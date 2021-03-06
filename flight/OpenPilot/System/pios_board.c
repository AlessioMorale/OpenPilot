/**
 ******************************************************************************
 * @addtogroup OpenPilotSystem OpenPilot System
 * @{
 * @addtogroup OpenPilotCore OpenPilot Core
 * @{
 *
 * @file       pios_board.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Defines board specific static initializers for hardware for the OpenPilot board.
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


/* Pull in the board-specific static HW definitions.
 * Including .c files is a bit ugly but this allows all of
 * the HW definitions to be const and static to limit their
 * scope.  
 *
 * NOTE: THIS IS THE ONLY PLACE THAT SHOULD EVER INCLUDE THIS FILE
 */
#include "board_hw_defs.c"

#include <pios.h>
#include <openpilot.h>
#include <uavobjectsinit.h>
#include <hwsettings.h>
#include <manualcontrolsettings.h>

//#define I2C_DEBUG_PIN			0
//#define USART_GPS_DEBUG_PIN		1

/* One slot per selectable receiver group.
 *  eg. PWM, PPM, GCS, DSMMAINPORT, DSMFLEXIPORT, SBUS
 * NOTE: No slot in this map for NONE.
 */
uint32_t pios_rcvr_group_map[MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE];

#define PIOS_COM_TELEM_RF_RX_BUF_LEN 192
#define PIOS_COM_TELEM_RF_TX_BUF_LEN 192

#define PIOS_COM_GPS_RX_BUF_LEN 96

#define PIOS_COM_TELEM_USB_RX_BUF_LEN 192
#define PIOS_COM_TELEM_USB_TX_BUF_LEN 192

#define PIOS_COM_BRIDGE_RX_BUF_LEN 65
#define PIOS_COM_BRIDGE_TX_BUF_LEN 12

uint32_t pios_com_telem_rf_id;
uint32_t pios_com_telem_usb_id;
uint32_t pios_com_vcp_id;
uint32_t pios_com_gps_id;
uint32_t pios_com_aux_id;
uint32_t pios_com_dsm_id;

#include "ahrs_spi_comm.h"

/**
 * PIOS_Board_Init()
 * initializes all the core subsystems on this specific hardware
 * called from System/openpilot.c
 */
void PIOS_Board_Init(void) {

	/* Remap AFIO pin */
	//GPIO_PinRemapConfig( GPIO_Remap_SWJ_NoJTRST, ENABLE);

#ifdef PIOS_DEBUG_ENABLE_DEBUG_PINS
	PIOS_DEBUG_Init(&pios_tim_servo_all_channels, NELEMENTS(pios_tim_servo_all_channels));
#endif	/* PIOS_DEBUG_ENABLE_DEBUG_PINS */

	/* Delay system */
	PIOS_DELAY_Init();	
	
#if defined(PIOS_INCLUDE_SPI)	
	/* Set up the SPI interface to the SD card */
	if (PIOS_SPI_Init(&pios_spi_sdcard_id, &pios_spi_sdcard_cfg)) {
		PIOS_Assert(0);
	}

	/* Enable and mount the SDCard */
	PIOS_SDCARD_Init(pios_spi_sdcard_id);
	PIOS_SDCARD_MountFS(0);
#endif /* PIOS_INCLUDE_SPI */

	/* Initialize UAVObject libraries */
	EventDispatcherInitialize();
	UAVObjInitialize();

#if defined(PIOS_INCLUDE_RTC)
	/* Initialize the real-time clock and its associated tick */
	PIOS_RTC_Init(&pios_rtc_main_cfg);
#endif

#if defined(PIOS_INCLUDE_LED)
	PIOS_LED_Init(&pios_led_cfg);
#endif	/* PIOS_INCLUDE_LED */

	HwSettingsInitialize();

	PIOS_WDG_Init();

	/* Initialize the alarms library */
	AlarmsInitialize();

	PIOS_IAP_Init();
	uint16_t boot_count = PIOS_IAP_ReadBootCount();
	if (boot_count < 3) {
		PIOS_IAP_WriteBootCount(++boot_count);
		AlarmsClear(SYSTEMALARMS_ALARM_BOOTFAULT);
	} else {
		/* Too many failed boot attempts, force hwsettings to defaults */
		HwSettingsSetDefaults(HwSettingsHandle(), 0);
		AlarmsSet(SYSTEMALARMS_ALARM_BOOTFAULT, SYSTEMALARMS_ALARM_CRITICAL);
	}

	/* Initialize the task monitor library */
	TaskMonitorInitialize();

	/* Set up pulse timers */
	PIOS_TIM_InitClock(&tim_1_cfg);
	PIOS_TIM_InitClock(&tim_3_cfg);
	PIOS_TIM_InitClock(&tim_5_cfg);
	PIOS_TIM_InitClock(&tim_4_cfg);
	PIOS_TIM_InitClock(&tim_8_cfg);

	/* Prepare the AHRS Comms upper layer protocol */
	AhrsInitComms();

	/* Set up the SPI interface to the AHRS */
	if (PIOS_SPI_Init(&pios_spi_ahrs_id, &pios_spi_ahrs_cfg)) {
		PIOS_Assert(0);
	}

	/* Bind the AHRS comms layer to the AHRS SPI link */
	AhrsConnect(pios_spi_ahrs_id);

#if defined(PIOS_INCLUDE_USB)
	/* Initialize board specific USB data */
	PIOS_USB_BOARD_DATA_Init();

	/* Flags to determine if various USB interfaces are advertised */
	bool usb_hid_present = false;
	bool usb_cdc_present = false;

	uint8_t hwsettings_usb_devicetype;
	HwSettingsUSB_DeviceTypeGet(&hwsettings_usb_devicetype);

	switch (hwsettings_usb_devicetype) {
	case HWSETTINGS_USB_DEVICETYPE_HIDONLY:
		if (PIOS_USB_DESC_HID_ONLY_Init()) {
			PIOS_Assert(0);
		}
		usb_hid_present = true;
		break;
	case HWSETTINGS_USB_DEVICETYPE_HIDVCP:
		if (PIOS_USB_DESC_HID_CDC_Init()) {
			PIOS_Assert(0);
		}
		usb_hid_present = true;
		usb_cdc_present = true;
		break;
	case HWSETTINGS_USB_DEVICETYPE_VCPONLY:
		break;
	default:
		PIOS_Assert(0);
	}

	uint32_t pios_usb_id;
	PIOS_USB_Init(&pios_usb_id, &pios_usb_main_cfg);

#if defined(PIOS_INCLUDE_USB_CDC)
	/* Configure the USB VCP port */
	uint8_t hwsettings_usb_vcpport;
	HwSettingsUSB_VCPPortGet(&hwsettings_usb_vcpport);

	if (!usb_cdc_present) {
		/* Force VCP port function to disabled if we haven't advertised VCP in our USB descriptor */
		hwsettings_usb_vcpport = HWSETTINGS_USB_VCPPORT_DISABLED;
	}

	switch (hwsettings_usb_vcpport) {
	case HWSETTINGS_USB_VCPPORT_DISABLED:
		break;
	case HWSETTINGS_USB_VCPPORT_USBTELEMETRY:
#if defined(PIOS_INCLUDE_COM)
		{
			uint32_t pios_usb_cdc_id;
			if (PIOS_USB_CDC_Init(&pios_usb_cdc_id, &pios_usb_cdc_cfg, pios_usb_id)) {
				PIOS_Assert(0);
			}
			uint8_t * rx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_TELEM_USB_RX_BUF_LEN);
			uint8_t * tx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_TELEM_USB_TX_BUF_LEN);
			PIOS_Assert(rx_buffer);
			PIOS_Assert(tx_buffer);
			if (PIOS_COM_Init(&pios_com_telem_usb_id, &pios_usb_cdc_com_driver, pios_usb_cdc_id,
						rx_buffer, PIOS_COM_TELEM_USB_RX_BUF_LEN,
						tx_buffer, PIOS_COM_TELEM_USB_TX_BUF_LEN)) {
				PIOS_Assert(0);
			}
		}
#endif	/* PIOS_INCLUDE_COM */
		break;
	case HWSETTINGS_USB_VCPPORT_COMBRIDGE:
#if defined(PIOS_INCLUDE_COM)
		{
			uint32_t pios_usb_cdc_id;
			if (PIOS_USB_CDC_Init(&pios_usb_cdc_id, &pios_usb_cdc_cfg, pios_usb_id)) {
				PIOS_Assert(0);
			}
			uint8_t * rx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_BRIDGE_RX_BUF_LEN);
			uint8_t * tx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_BRIDGE_TX_BUF_LEN);
			PIOS_Assert(rx_buffer);
			PIOS_Assert(tx_buffer);
			if (PIOS_COM_Init(&pios_com_vcp_id, &pios_usb_cdc_com_driver, pios_usb_cdc_id,
						rx_buffer, PIOS_COM_BRIDGE_RX_BUF_LEN,
						tx_buffer, PIOS_COM_BRIDGE_TX_BUF_LEN)) {
				PIOS_Assert(0);
			}

		}
#endif	/* PIOS_INCLUDE_COM */
		break;
	}
#endif	/* PIOS_INCLUDE_USB_CDC */

#if defined(PIOS_INCLUDE_USB_HID)
	/* Configure the usb HID port */
	uint8_t hwsettings_usb_hidport;
	HwSettingsUSB_HIDPortGet(&hwsettings_usb_hidport);

	if (!usb_hid_present) {
		/* Force HID port function to disabled if we haven't advertised HID in our USB descriptor */
		hwsettings_usb_hidport = HWSETTINGS_USB_HIDPORT_DISABLED;
	}

	switch (hwsettings_usb_hidport) {
	case HWSETTINGS_USB_HIDPORT_DISABLED:
		break;
	case HWSETTINGS_USB_HIDPORT_USBTELEMETRY:
#if defined(PIOS_INCLUDE_COM)
		{
			uint32_t pios_usb_hid_id;
			if (PIOS_USB_HID_Init(&pios_usb_hid_id, &pios_usb_hid_cfg, pios_usb_id)) {
				PIOS_Assert(0);
			}
			uint8_t * rx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_TELEM_USB_RX_BUF_LEN);
			uint8_t * tx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_TELEM_USB_TX_BUF_LEN);
			PIOS_Assert(rx_buffer);
			PIOS_Assert(tx_buffer);
			if (PIOS_COM_Init(&pios_com_telem_usb_id, &pios_usb_hid_com_driver, pios_usb_hid_id,
						rx_buffer, PIOS_COM_TELEM_USB_RX_BUF_LEN,
						tx_buffer, PIOS_COM_TELEM_USB_TX_BUF_LEN)) {
				PIOS_Assert(0);
			}
		}
#endif	/* PIOS_INCLUDE_COM */
		break;
	}

#endif	/* PIOS_INCLUDE_USB_HID */

#endif	/* PIOS_INCLUDE_USB */

	/* Configure the main IO port */
	uint8_t hwsettings_op_mainport;
	HwSettingsOP_MainPortGet(&hwsettings_op_mainport);

	switch (hwsettings_op_mainport) {
	case HWSETTINGS_OP_MAINPORT_DISABLED:
		break;
	case HWSETTINGS_OP_MAINPORT_TELEMETRY:
#if defined(PIOS_INCLUDE_TELEMETRY_RF)
		{
			uint32_t pios_usart_telem_rf_id;
			if (PIOS_USART_Init(&pios_usart_telem_rf_id, &pios_usart_telem_cfg)) {
				PIOS_Assert(0);
			}

			uint8_t * rx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_TELEM_RF_RX_BUF_LEN);
			uint8_t * tx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_TELEM_RF_TX_BUF_LEN);
			PIOS_Assert(rx_buffer);
			PIOS_Assert(tx_buffer);
			if (PIOS_COM_Init(&pios_com_telem_rf_id, &pios_usart_com_driver, pios_usart_telem_rf_id,
							  rx_buffer, PIOS_COM_TELEM_RF_RX_BUF_LEN,
							  tx_buffer, PIOS_COM_TELEM_RF_TX_BUF_LEN)) {
				PIOS_Assert(0);
			}
		}
#endif /* PIOS_INCLUDE_TELEMETRY_RF */
		break;
	}

	/* Configure the flexi port */
	uint8_t hwsettings_op_flexiport;
	HwSettingsOP_FlexiPortGet(&hwsettings_op_flexiport);

	switch (hwsettings_op_flexiport) {
	case HWSETTINGS_OP_FLEXIPORT_DISABLED:
		break;
	case HWSETTINGS_OP_FLEXIPORT_GPS:
#if defined(PIOS_INCLUDE_GPS)
		{
			uint32_t pios_usart_gps_id;
			if (PIOS_USART_Init(&pios_usart_gps_id, &pios_usart_gps_cfg)) {
				PIOS_Assert(0);
			}
			uint8_t * rx_buffer = (uint8_t *) pvPortMalloc(PIOS_COM_GPS_RX_BUF_LEN);
			PIOS_Assert(rx_buffer);
			if (PIOS_COM_Init(&pios_com_gps_id, &pios_usart_com_driver, pios_usart_gps_id,
						rx_buffer, PIOS_COM_GPS_RX_BUF_LEN,
						NULL, 0)) {
				PIOS_Assert(0);
			}
		}
#endif	/* PIOS_INCLUDE_GPS */
		break;
	}

#ifndef PIOS_DEBUG_ENABLE_DEBUG_PINS
	PIOS_Servo_Init(&pios_servo_cfg);
#endif	/* PIOS_DEBUG_ENABLE_DEBUG_PINS */

	PIOS_ADC_Init();
	PIOS_GPIO_Init();

	/* Configure the rcvr port */
	uint8_t hwsettings_rcvrport;
	HwSettingsOP_RcvrPortGet(&hwsettings_rcvrport);


	switch (hwsettings_rcvrport) {
	case HWSETTINGS_OP_RCVRPORT_DISABLED:
		break;
	case HWSETTINGS_OP_RCVRPORT_DEBUG:
		/* Not supported yet */
		break;
	case HWSETTINGS_OP_RCVRPORT_DSM2:
	case HWSETTINGS_OP_RCVRPORT_DSMX10BIT:
	case HWSETTINGS_OP_RCVRPORT_DSMX11BIT:
#if defined(PIOS_INCLUDE_DSM)
		{
			enum pios_dsm_proto proto;
			switch (hwsettings_rcvrport) {
			case HWSETTINGS_OP_RCVRPORT_DSM2:
				proto = PIOS_DSM_PROTO_DSM2;
				break;
			case HWSETTINGS_OP_RCVRPORT_DSMX10BIT:
				proto = PIOS_DSM_PROTO_DSMX10BIT;
				break;
			case HWSETTINGS_OP_RCVRPORT_DSMX11BIT:
				proto = PIOS_DSM_PROTO_DSMX11BIT;
				break;
			default:
				PIOS_Assert(0);
				break;
			}

			uint32_t pios_usart_dsm_id;
			if (PIOS_USART_Init(&pios_usart_dsm_id, &pios_usart_dsm_cfg)) {
				PIOS_Assert(0);
			}

			uint32_t pios_dsm_id;
			if (PIOS_DSM_Init(&pios_dsm_id,
					  &pios_dsm_cfg,
					  &pios_usart_com_driver,
					  pios_usart_dsm_id,
					  proto, 0)) {
				PIOS_Assert(0);
			}

			uint32_t pios_dsm_rcvr_id;
			if (PIOS_RCVR_Init(&pios_dsm_rcvr_id, &pios_dsm_rcvr_driver, pios_dsm_id)) {
				PIOS_Assert(0);
			}
			pios_rcvr_group_map[MANUALCONTROLSETTINGS_CHANNELGROUPS_DSMMAINPORT] = pios_dsm_rcvr_id;
		}
#endif
		break;
	case HWSETTINGS_OP_RCVRPORT_PWM:
#if defined(PIOS_INCLUDE_PWM)
		{
			uint32_t pios_pwm_id;
			PIOS_PWM_Init(&pios_pwm_id, &pios_pwm_cfg);

			uint32_t pios_pwm_rcvr_id;
			if (PIOS_RCVR_Init(&pios_pwm_rcvr_id, &pios_pwm_rcvr_driver, pios_pwm_id)) {
				PIOS_Assert(0);
			}
			pios_rcvr_group_map[MANUALCONTROLSETTINGS_CHANNELGROUPS_PWM] = pios_pwm_rcvr_id;
		}
#endif	/* PIOS_INCLUDE_PWM */
		break;
	case HWSETTINGS_OP_RCVRPORT_PPM:
#if defined(PIOS_INCLUDE_PPM)
		{
			uint32_t pios_ppm_id;
			PIOS_PPM_Init(&pios_ppm_id, &pios_ppm_cfg);

			uint32_t pios_ppm_rcvr_id;
			if (PIOS_RCVR_Init(&pios_ppm_rcvr_id, &pios_ppm_rcvr_driver, pios_ppm_id)) {
				PIOS_Assert(0);
			}
			pios_rcvr_group_map[MANUALCONTROLSETTINGS_CHANNELGROUPS_PPM] = pios_ppm_rcvr_id;
		}
#endif	/* PIOS_INCLUDE_PPM */
		break;
	}

#if defined(PIOS_INCLUDE_I2C)
	if (PIOS_I2C_Init(&pios_i2c_main_adapter_id, &pios_i2c_main_adapter_cfg)) {
		PIOS_Assert(0);
	}
#endif	/* PIOS_INCLUDE_I2C */

	/* Make sure we have at least one telemetry link configured or else fail initialization */
	PIOS_Assert(pios_com_telem_rf_id || pios_com_telem_usb_id);
}

/**
 * @}
 */
