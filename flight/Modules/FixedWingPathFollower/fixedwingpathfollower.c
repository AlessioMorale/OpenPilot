/**
 ******************************************************************************
 *
 * @file       fixedwingpathfollower.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      This module compared @ref PositionActuatl to @ref ActiveWaypoint 
 * and sets @ref AttitudeDesired.  It only does this when the FlightMode field
 * of @ref ManualControlCommand is Auto.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input object: ActiveWaypoint
 * Input object: PositionActual
 * Input object: ManualControlCommand
 * Output object: AttitudeDesired
 *
 * This module will periodically update the value of the AttitudeDesired object.
 *
 * The module executes in its own thread in this example.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "openpilot.h"
#include "paths.h"

#include "fixedwingpathfollower.h"
#include "accels.h"
#include "hwsettings.h"
#include "attitudeactual.h"
#include "pathdesired.h"	// object that will be updated by the module
#include "positionactual.h"
#include "manualcontrol.h"
#include "flightstatus.h"
#include "pathstatus.h"
#include "baroairspeed.h"
#include "gpsvelocity.h"
#include "gpsposition.h"
#include "fixedwingpathfollowersettings.h"
#include "fixedwingpathfollowerstatus.h"
#include "homelocation.h"
#include "nedposition.h"
#include "stabilizationdesired.h"
#include "stabilizationsettings.h"
#include "systemsettings.h"
#include "velocitydesired.h"
#include "velocityactual.h"
#include "CoordinateConversions.h"

// Private constants
#define MAX_QUEUE_SIZE 4
#define STACK_SIZE_BYTES 1548
#define TASK_PRIORITY (tskIDLE_PRIORITY+2)
#define F_PI 3.14159265358979323846f
#define RAD2DEG (180.0f/F_PI)
#define GEE 9.81f
// Private types

// Private variables
static bool followerEnabled = false;
static xTaskHandle pathfollowerTaskHandle;
static xQueueHandle queue;
static FixedWingPathFollowerSettingsData fixedwingpathfollowerSettings;

// Private functions
static void pathfollowerTask(void *parameters);
static void SettingsUpdatedCb(UAVObjEvent * ev);
static void updatePathVelocity();
static void updateEndpointVelocity();
static void updateFixedDesiredAttitude();
static void updateFixedFixedAttitude();
static void baroAirspeedUpdatedCb(UAVObjEvent * ev);
static float bound(float val, float min, float max);

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t FixedWingPathFollowerStart()
{
	if (followerEnabled) {
		// Start main task
		xTaskCreate(pathfollowerTask, (signed char *)"PathFollower", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &pathfollowerTaskHandle);
		TaskMonitorAdd(TASKINFO_RUNNING_PATHFOLLOWER, pathfollowerTaskHandle);
	}

	return 0;
}

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t FixedWingPathFollowerInitialize()
{
	HwSettingsInitialize();
	uint8_t optionalModules[HWSETTINGS_OPTIONALMODULES_NUMELEM];
	HwSettingsOptionalModulesGet(optionalModules);
	if (optionalModules[HWSETTINGS_OPTIONALMODULES_VTOLPATHFOLLOWER] == HWSETTINGS_OPTIONALMODULES_ENABLED) {
		followerEnabled = true;
		FixedWingPathFollowerSettingsInitialize();
		FixedWingPathFollowerStatusInitialize();
		PathDesiredInitialize();
		PathStatusInitialize();
		VelocityDesiredInitialize();
		BaroAirspeedInitialize();
	} else {
		followerEnabled = false;
	}
	return 0;
}
MODULE_INITCALL(FixedWingPathFollowerInitialize, FixedWingPathFollowerStart)

static float northVelIntegral = 0;
static float eastVelIntegral = 0;
static float downVelIntegral = 0;

static float courseIntegral = 0;
static float speedIntegral = 0;
static float accelIntegral = 0;
static float powerIntegral = 0;
static uint8_t positionHoldLast = 0;

// correct speed by measured airspeed
static float baroAirspeedBias = 0;

/**
 * Module thread, should not return.
 */
static void pathfollowerTask(void *parameters)
{
	SystemSettingsData systemSettings;
	FlightStatusData flightStatus;
	PathStatusData pathStatus;
	
	portTickType lastUpdateTime;
	
	BaroAirspeedConnectCallback(baroAirspeedUpdatedCb);
	FixedWingPathFollowerSettingsConnectCallback(SettingsUpdatedCb);
	PathDesiredConnectCallback(SettingsUpdatedCb);
	
	FixedWingPathFollowerSettingsGet(&fixedwingpathfollowerSettings);
	PathDesiredGet(&pathDesired);
	
	// Main task loop
	lastUpdateTime = xTaskGetTickCount();
	while (1) {

		// Conditions when this runs:
		// 1. Must have FixedWing type airframe
		// 2. Flight mode is PositionHold and PathDesired.Mode is Endpoint  OR
		//    FlightMode is PathPlanner and PathDesired.Mode is Endpoint or Path

		SystemSettingsGet(&systemSettings);
		if ( (systemSettings.AirframeType != SYSTEMSETTINGS_AIRFRAMETYPE_FIXEDWING) &&
			(systemSettings.AirframeType != SYSTEMSETTINGS_AIRFRAMETYPE_FIXEDWINGELEVON) &&
			(systemSettings.AirframeType != SYSTEMSETTINGS_AIRFRAMETYPE_FIXEDWINGVTAIL) )
		{
			AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_WARNING);
			vTaskDelay(1000);
			continue;
		}

		// Continue collecting data if not enough time
		vTaskDelayUntil(&lastUpdateTime, vtolpathfollowerSettings.UpdatePeriod / portTICK_RATE_MS);

		
		FlightStatusGet(&flightStatus);
		PathStatusGet(&pathStatus);
		
		// Check the combinations of flightmode and pathdesired mode
		switch(flightStatus.FlightMode) {
			case FLIGHTSTATUS_FLIGHTMODE_POSITIONHOLD:
			case FLIGHTSTATUS_FLIGHTMODE_RETURNTOBASE:
				if (pathDesired.Mode == PATHDESIRED_MODE_FLYENDPOINT) {
					updateEndpointVelocity();
					updateVtolDesiredAttitude();
					AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_OK);
				} else {
					AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_ERROR);
				}
				break;
			case FLIGHTSTATUS_FLIGHTMODE_PATHPLANNER:
				pathStatus.UID = pathDesired.UID;
				pathStatus.Status = PATHSTATUS_STATUS_INPROGRESS;
				switch(pathDesired.Mode) {
					// TODO: Make updateVtolDesiredAttitude and velocity report success and update PATHSTATUS_STATUS accordingly
					case PATHDESIRED_MODE_FLYENDPOINT:
						updateEndpointVelocity();
						updateFixedDesiredAttitude();
						AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_OK);
						break;
					case PATHDESIRED_MODE_FLYVECTOR:
						updatePathVelocity();
						updateFixedDesiredAttitude();
						AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_OK);
						break;
					case PATHDESIRED_MODE_FIXEDATTITUDE:
						updateFixedAttitude(pathDesired.ModeParameters);
						AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_OK);
						break;
					case PATHDESIRED_MODE_DISARMALARM:
						AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_CRITICAL);
						break;
					default:
						pathStatus.Status = PATHSTATUS_STATUS_CRITICAL;
						AlarmsSet(SYSTEMALARMS_ALARM_GUIDANCE,SYSTEMALARMS_ALARM_ERROR);
						break;
				}
				break;
			default:
				// Be cleaner and get rid of global variables
				northVelIntegral = 0;
				eastVelIntegral = 0;
				downVelIntegral = 0;
				courseIntegral = 0;
				speedIntegral = 0;
				accelIntegral = 0;
				powerIntegral = 0;

				break;
		}
	}
}

/**
 * Compute desired velocity from the current position and path
 *
 * Takes in @ref PositionActual and compares it to @ref PathDesired 
 * and computes @ref VelocityDesired
 */
static void updatePathVelocity()
{
	float dT = vtolpathfollowerSettings.UpdatePeriod / 1000.0f;
	float downCommand;

	PositionActualData positionActual;
	PositionActualGet(&positionActual);
	
	float cur[3] = {positionActual.North, positionActual.East, positionActual.Down};
	struct path_status progress;
	
	path_progress(pathDesired.Start, pathDesired.End, cur, &progress);
	
	float groundspeed = pathDesired.StartingVelocity + 
	    (pathDesired.EndingVelocity - pathDesired.StartingVelocity) * progress.fractional_progress;
	if(progress.fractional_progress > 1)
		groundspeed = pathDesired.EndingVelocity;
	
	VelocityDesiredData velocityDesired;
	velocityDesired.North = progress.path_direction[0] * groundspeed;
	velocityDesired.East = progress.path_direction[1] * groundspeed;
	
	float error_speed = progress.error * vtolpathfollowerSettings.HorizontalPosP;
	float correction_velocity[2] = {progress.correction_direction[0] * error_speed, 
	    progress.correction_direction[1] * error_speed};
	
	// prevent div by zero
	if (fabsf(correction_velocity[0])+fabsf(correction_velocity[1]) <1e-6) {
		correction_velocity[0]=1e-6;
	}

	float total_vel = sqrtf(powf(correction_velocity[0],2) + powf(correction_velocity[1],2));
	float scale = 1;
	if(total_vel > vtolpathfollowerSettings.HorizontalVelMax)
		scale = vtolpathfollowerSettings.HorizontalVelMax / total_vel;
	if (total_vel < vtolpathfollowerSettings.HorizontalVelMin)
		scale = vtolpathfollowerSettings.HorizontalVelMin / total_vel;

	velocityDesired.North += progress.correction_direction[0] * error_speed * scale;
	velocityDesired.East += progress.correction_direction[1] * error_speed * scale;
	
	float altitudeSetpoint = pathDesired.Start[2] + (pathDesired.End[2] - pathDesired.Start[2]) *
	    bound(progress.fractional_progress,0,1);

	float downError = altitudeSetpoint - positionActual.Down;
	downCommand = downError * vtolpathfollowerSettings.VerticalPosP;
	velocityDesired.Down = bound(downCommand,
					 -vtolpathfollowerSettings.VerticalVelMax,
					 vtolpathfollowerSettings.VerticalVelMax);

	VelocityDesiredSet(&velocityDesired);
}

/**
 * Compute desired velocity from the current position
 *
 * Takes in @ref PositionActual and compares it to @ref PositionDesired 
 * and computes @ref VelocityDesired
 */
void updateEndpointVelocity()
{
	float dT = vtolpathfollowerSettings.UpdatePeriod / 1000.0f;

	PositionActualData positionActual;
	VelocityDesiredData velocityDesired;
	
	PositionActualGet(&positionActual);
	VelocityDesiredGet(&velocityDesired);
	
	float northError;
	float eastError;
	float downError;
	float northCommand;
	float eastCommand;
	float downCommand;
	
	float northPos = 0, eastPos = 0, downPos = 0;
	northPos = positionActual.North;
	eastPos = positionActual.East;
	downPos = positionActual.Down;

	// Compute commands
	northError = pathDesired.End[PATHDESIRED_END_NORTH] - positionActual.North;
	northCommand = northError * vtolpathfollowerSettings.HorizontalPosP;

	eastError = pathDesired.End[PATHDESIRED_END_EAST] - eastPos;
	eastCommand = eastError * vtolpathfollowerSettings.HorizontalPosP;
	
	// prevent div by zero
	if (fabsf(northCommand)+fabsf(eastCommand) <1e-6) {
		nortCommand=1e-6;
	}
	
	// Limit the maximum velocity
	float total_vel = sqrtf(powf(northCommand,2) + powf(eastCommand,2));
	float scale = 1;
	if(total_vel > vtolpathfollowerSettings.HorizontalVelMax)
		scale = vtolpathfollowerSettings.HorizontalVelMax / total_vel;
	if (total_vel < vtolpathfollowerSettings.HorizontalVelMin)
		scale = vtolpathfollowerSettings.HorizontalVelMin / total_vel;

	velocityDesired.North = northCommand * scale;
	velocityDesired.East = eastCommand * scale;

	downError = pathDesired.End[PATHDESIRED_END_DOWN] - downPos;
	downCommand = downError * vtolpathfollowerSettings.VerticalPosP;
	velocityDesired.Down = bound(downCommand,
				     -vtolpathfollowerSettings.VerticalVelMax, 
				     vtolpathfollowerSettings.VerticalVelMax);
	
	VelocityDesiredSet(&velocityDesired);	
}

/**
 * Compute desired attitude from a fixed preset
 *
 */
static void updateFixedAttitude(float* attitude)
{
	StabilizationDesiredData stabDesired;
	StabilizationDesiredGet(&stabDesired);
	stabDesired.Roll     = attitude[0];
	stabDesired.Pitch    = attitude[1];
	stabDesired.Yaw      = attitude[2];
	stabDesired.Throttle = attitude[3];
	stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_ROLL] = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
	stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_PITCH] = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
	stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_YAW] = STABILIZATIONDESIRED_STABILIZATIONMODE_RATE;
	StabilizationDesiredSet(&stabDesired);
}

/**
 * Compute desired attitude from the desired velocity
 *
 * Takes in @ref NedActual which has the acceleration in the 
 * NED frame as the feedback term and then compares the 
 * @ref VelocityActual against the @ref VelocityDesired
 */
static void updateFixedDesiredAttitude()
{
	float dT = vtolpathfollowerSettings.UpdatePeriod / 1000.0f;

	VelocityDesiredData velocityDesired;
	VelocityActualData velocityActual;
	StabilizationDesiredData stabDesired;
	AttitudeActualData attitudeActual;
	AccelsData accels;
	FixedWingPathFollowerSettingsData fixedwingpathfollowerSettings;
	StabilizationSettingsData stabSettings;
	FixedWingPathFollowerStatusData fixedwingpathfollowerStatus;

	float courseError;
	float courseCommand;

	float speedError;
	float accelCommand;

	float speedActual;
	float speedDesired;
	float accelDesired;
	float accelError;

	float powerError;
	float powerCommand;

	FixedWingPathFollowerSettingsGet(&fixedwingpathfollowerSettings);

	FixedWingPathFollowerStatusGet(&fixedwingpathfollowerStatus);
	
	VelocityActualGet(&velocityActual);
	VelocityDesiredGet(&velocityDesired);
	StabilizationDesiredGet(&stabDesired);
	VelocityDesiredGet(&velocityDesired);
	AttitudeActualGet(&attitudeActual);
	AccelsGet(&accels);
	StabilizationSettingsGet(&stabSettings);

	// current speed - lacking forward airspeed we use groundspeed :(
	speedActual = sqrtf(velocityActual.East*velocityActual.East + velocityActual.North*velocityActual.North + velocityActual.Down*velocityActual.Down ) + baroAirspeedBias;

	// Compute desired roll command
	courseError = RAD2DEG * (atan2f(velocityDesired.East,velocityDesired.North) - atan2f(velocityActual.East,velocityActual.North));
	if (courseError<-180.0f) courseError+=360.0f;
	if (courseError>180.0f) courseError-=360.0f;

	courseIntegral = bound(courseIntegral + courseError * dT * fixedwingpathfollowerSettings.CoursePI[GUIDANCESETTINGS_COURSEPI_KI], 
		-fixedwingpathfollowerSettings.CoursePI[GUIDANCESETTINGS_COURSEPI_ILIMIT],
		fixedwingpathfollowerSettings.CoursePI[GUIDANCESETTINGS_COURSEPI_ILIMIT]);
	courseCommand = (courseError * fixedwingpathfollowerSettings.CoursePI[GUIDANCESETTINGS_COURSEPI_KP] +
		courseIntegral);

	fixedwingpathfollowerStatus.E[GUIDANCESTATUS_E_COURSE] = courseError;
	fixedwingpathfollowerStatus.A[GUIDANCESTATUS_A_COURSE] = courseIntegral;
	fixedwingpathfollowerStatus.C[GUIDANCESTATUS_C_COURSE] = courseCommand;
	
	stabDesired.Roll = bound( fixedwingpathfollowerSettings.RollLimit[GUIDANCESETTINGS_ROLLLIMIT_NEUTRAL] +
		courseCommand,
		fixedwingpathfollowerSettings.RollLimit[GUIDANCESETTINGS_ROLLLIMIT_MIN],
		fixedwingpathfollowerSettings.RollLimit[GUIDANCESETTINGS_ROLLLIMIT_MAX] );

	// Compute desired yaw command
	// TODO implement raw control mode for yaw and base on Accels.X
	stabDesired.Yaw = 0;

	// Compute desired speed command  TODO: make cruise speed a variable
	speedDesired = fixedwingpathfollowerSettings.CruiseSpeed;
	speedError = speedDesired - speedActual;

	accelDesired = bound( speedError * fixedwingpathfollowerSettings.SpeedP[GUIDANCESETTINGS_SPEEDP_KP],
		-fixedwingpathfollowerSettings.SpeedP[GUIDANCESETTINGS_SPEEDP_MAX],
		fixedwingpathfollowerSettings.SpeedP[GUIDANCESETTINGS_SPEEDP_MAX]);
	
	fixedwingpathfollowerStatus.E[GUIDANCESTATUS_E_SPEED] = speedError;
	fixedwingpathfollowerStatus.A[GUIDANCESTATUS_A_SPEED] = 0.0f;
	fixedwingpathfollowerStatus.C[GUIDANCESTATUS_C_SPEED] = accelDesired;
	
	accelError = accelDesired - accels.x;
	accelIntegral = bound(accelIntegral + accelError * dT * fixedwingpathfollowerSettings.AccelPI[GUIDANCESETTINGS_ACCELPI_KI], 
		-fixedwingpathfollowerSettings.AccelPI[GUIDANCESETTINGS_ACCELPI_ILIMIT],
		fixedwingpathfollowerSettings.AccelPI[GUIDANCESETTINGS_ACCELPI_ILIMIT]);
	accelCommand = (accelError * fixedwingpathfollowerSettings.AccelPI[GUIDANCESETTINGS_ACCELPI_KP] + 
		 accelIntegral);
	
	fixedwingpathfollowerStatus.E[GUIDANCESTATUS_E_ACCEL] = accelError;
	fixedwingpathfollowerStatus.A[GUIDANCESTATUS_A_ACCEL] = accelIntegral;
	fixedwingpathfollowerStatus.C[GUIDANCESTATUS_C_ACCEL] = accelCommand;

	stabDesired.Pitch = bound(fixedwingpathfollowerSettings.PitchLimit[GUIDANCESETTINGS_PITCHLIMIT_NEUTRAL] +
		-accelCommand,
		fixedwingpathfollowerSettings.PitchLimit[GUIDANCESETTINGS_PITCHLIMIT_MIN],
		fixedwingpathfollowerSettings.PitchLimit[GUIDANCESETTINGS_PITCHLIMIT_MAX]);

	// Compute desired power command
	powerError =  -( velocityDesired.Down - velocityActual.Down ) * fixedwingpathfollowerSettings.ClimbRateBoostFactor + speedError;
	powerIntegral =	bound(powerIntegral + powerError * dT * fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_KI], 
		-fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_ILIMIT],
		fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_ILIMIT]);
	powerCommand = (powerError * fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_KP] +
		powerIntegral) + fixedwingpathfollowerSettings.ThrottleLimit[GUIDANCESETTINGS_THROTTLELIMIT_NEUTRAL];

	// prevent integral running out of bounds 
	if ( powerCommand > fixedwingpathfollowerSettings.ThrottleLimit[GUIDANCESETTINGS_THROTTLELIMIT_MAX]) {
		powerIntegral = bound(
			powerIntegral -
				( powerCommand 
				- fixedwingpathfollowerSettings.ThrottleLimit[GUIDANCESETTINGS_THROTTLELIMIT_MAX]),
			-fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_ILIMIT],
			fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_ILIMIT]);
		powerCommand = fixedwingpathfollowerSettings.ThrottleLimit[GUIDANCESETTINGS_THROTTLELIMIT_MAX];
	}
	if ( powerCommand < fixedwingpathfollowerSettings.ThrottleLimit[GUIDANCESETTINGS_THROTTLELIMIT_MIN]) {
		powerIntegral = bound(
			powerIntegral -
				( powerCommand
				- fixedwingpathfollowerSettings.ThrottleLimit[GUIDANCESETTINGS_THROTTLELIMIT_MIN]),
			-fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_ILIMIT],
			fixedwingpathfollowerSettings.PowerPI[GUIDANCESETTINGS_POWERPI_ILIMIT]);
		powerCommand = fixedwingpathfollowerSettings.ThrottleLimit[GUIDANCESETTINGS_THROTTLELIMIT_MIN];
	}

	fixedwingpathfollowerStatus.E[GUIDANCESTATUS_E_POWER] = powerError;
	fixedwingpathfollowerStatus.A[GUIDANCESTATUS_A_POWER] = powerIntegral;
	fixedwingpathfollowerStatus.C[GUIDANCESTATUS_C_POWER] = powerCommand;

	// set throttle
	stabDesired.Throttle = powerCommand;

	if(fixedwingpathfollowerSettings.ThrottleControl == GUIDANCESETTINGS_THROTTLECONTROL_FALSE) {
		// For now override throttle with manual control.  Disable at your risk, quad goes to China.
		ManualControlCommandData manualControl;
		ManualControlCommandGet(&manualControl);
		stabDesired.Throttle = manualControl.Throttle;
	}
//printf("Cycle:	speed Error: %f\n	powerError: %f\n	accelCommand: %f\n	powerCommand: %f\n\n",speedError,powerError,accelCommand,powerCommand);

	stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_ROLL] = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
	stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_PITCH] = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
	stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_YAW] = STABILIZATIONDESIRED_STABILIZATIONMODE_MANUAL;
	
	StabilizationDesiredSet(&stabDesired);

	FixedWingPathFollowerStatusSet(&fixedwingpathfollowerStatus);
}


/**
 * Bound input value between limits
 */
static float bound(float val, float min, float max)
{
	if (val < min) {
		val = min;
	} else if (val > max) {
		val = max;
	}
	return val;
}

static void SettingsUpdatedCb(UAVObjEvent * ev)
{
	VtolPathFollowerSettingsGet(&vtolpathfollowerSettings);
	PathDesiredGet(&pathDesired);
}

static void baroAirspeedUpdatedCb(UAVObjEvent * ev)
{

	BaroAirspeedData baroAirspeed;
	VelocityActualData velocityActual;

	BaroAirspeedGet(&baroAirspeed);
	if (baroAirspeed.Connected != BAROAIRSPEED_CONNECTED_TRUE) {
		baroAirspeedBias = 0;
	} else {
		VelocityActualGet(&velocityActual);
		float speed = sqrtf(velocityActual.East*velocityActual.East + velocityActual.North*velocityActual.North + velocityActual.Down*velocityActual.Down );

		baroAirspeedBias = baroAirspeed.Airspeed - speed;
	}

}
