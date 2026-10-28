/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "mk_app.h"
#include "gps_raw.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for uart_bluetooth */
osThreadId_t uart_bluetoothHandle;
const osThreadAttr_t uart_bluetooth_attributes = {
  .name = "uart_bluetooth",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for led_blink */
osThreadId_t led_blinkHandle;
const osThreadAttr_t led_blink_attributes = {
  .name = "led_blink",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for mavlinkTask */
osThreadId_t mavlinkTaskHandle;
const osThreadAttr_t mavlinkTask_attributes = {
  .name = "mavlinkTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for gpsTask */
osThreadId_t gpsTaskHandle;
const osThreadAttr_t gpsTask_attributes = {
  .name = "gpsTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void BLUETOOTH(void *argument);
void led_blink1(void *argument);
void StartMavlinkTask(void *argument);
void StartGpsTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  MAVLink_AppInit();
  GPS_RawInit();
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of uart_bluetooth */
  uart_bluetoothHandle = osThreadNew(BLUETOOTH, NULL, &uart_bluetooth_attributes);

  /* creation of led_blink */
  led_blinkHandle = osThreadNew(led_blink1, NULL, &led_blink_attributes);

  /* creation of mavlinkTask */
  mavlinkTaskHandle = osThreadNew(StartMavlinkTask, NULL, &mavlinkTask_attributes);

  /* creation of gpsTask */
  gpsTaskHandle = osThreadNew(StartGpsTask, NULL, &gpsTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_BLUETOOTH */
/**
* @brief Function implementing the uart_bluetooth thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_BLUETOOTH */
void BLUETOOTH(void *argument)
{
  /* USER CODE BEGIN BLUETOOTH */
    char buffer[192];
    FC_Telemetry_t fc = {0};
    static GPS_RawSnapshot_t gps;
    uint32_t printed_gsa_count = 0;
    uint32_t printed_rmc_count = 0;
    uint32_t previous_pvt_count = 0, previous_tx_count = 0, previous_print_ms = 0;

    for (;;)
    {
        int length = 0;
        bool telemetry_available = false;

        if (MAVLink_GetTelemetry(&fc))
        {
            telemetry_available = true;
            uint32_t now = (uint32_t)(xTaskGetTickCount()
                                      * portTICK_PERIOD_MS);
            uint32_t attitude_age = now - fc.last_attitude_ms;
            const char *arm = fc.connected
                ? (fc.armed ? "ARMED" : "DISARMED") : "UNKNOWN";

            if (fc.attitude_count == 0U)
            {
                length = snprintf(
                    buffer, sizeof(buffer),
                    "Arm=%s NO ATTITUDE RX=%lu HB=%lu ATT=0 ERR=%lu\r\n",
                    arm,
                    (unsigned long)MAVLink_GetRxByteCount(),
                    (unsigned long)fc.heartbeat_count,
                    (unsigned long)huart1.ErrorCode
                );
            }
            else if (attitude_age >= 1000U)
            {
                length = snprintf(
                    buffer, sizeof(buffer),
                    "Arm=%s ATTITUDE STALE age=%lu ms HB=%lu ATT=%lu\r\n",
                    arm, (unsigned long)attitude_age,
                    (unsigned long)fc.heartbeat_count,
                    (unsigned long)fc.attitude_count
                );
            }
            else
            {
                // Whole degrees; integer formatting avoids printf float support.
                long yaw_deg = (long)(fc.yaw * 57.2957795f);
                long roll_deg = (long)(fc.roll * 57.2957795f);

                length = snprintf(
                    buffer, sizeof(buffer),
                    "Yaw=%ld deg Roll=%ld deg Arm=%s\r\n",
                    yaw_deg, roll_deg,
                    arm
                );
            }
        }
        else
        {
            length = snprintf(buffer, sizeof(buffer),
                              "Telemetry read failed\r\n");
        }

        if (length > 0 && (size_t)length < sizeof(buffer))
        {
            HAL_UART_Transmit(&huart7, (uint8_t *)buffer,
                              (uint16_t)length, 100);
        }

        if (telemetry_available)
        {
            length = snprintf(buffer, sizeof(buffer),
                "GPS TX=%lu ERR=%lu LastFix=%u\r\n",
                (unsigned long)fc.gps_tx_count, (unsigned long)fc.gps_tx_errors,
                (unsigned int)fc.gps_tx_fix_type);
            if (length > 0 && (size_t)length < sizeof(buffer))
                HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);
        }

        if (GPS_RawGetSnapshot(&gps))
        {
            uint32_t now = (uint32_t)(xTaskGetTickCount()
                                      * portTICK_PERIOD_MS);
            static const char *const config_names[] = {"WAIT", "SENT", "ACK", "NAK", "TIMEOUT"};
            const char *config = (unsigned)gps.config_state < 5U
                ? config_names[gps.config_state] : "UNKNOWN";
            if (gps.pvt_count != 0U)
            {
                uint32_t elapsed = now - previous_print_ms;
                uint32_t rx_rate = 0, tx_rate = 0;
                if (previous_print_ms != 0U && elapsed != 0U)
                {
                    rx_rate = (uint32_t)((uint64_t)(gps.pvt_count - previous_pvt_count) * 10000ULL / elapsed);
                    if (telemetry_available)
                        tx_rate = (uint32_t)((uint64_t)(fc.gps_tx_count - previous_tx_count) * 10000ULL / elapsed);
                }
                previous_print_ms = now;
                previous_pvt_count = gps.pvt_count;
                previous_tx_count = fc.gps_tx_count;
                uint32_t pvt_age = now - gps.last_pvt_ms;
                length = snprintf(buffer, sizeof(buffer),
                    "GPS PVT=%lu DOP=%lu RX=%lu.%luHz TX=%lu.%luHz Step=%lums Age=%lums Bad=%lu Dup=%lu Gap=%lu CFG=%s\r\n",
                    (unsigned long)gps.pvt_count, (unsigned long)gps.dop_count,
                    (unsigned long)(rx_rate / 10U), (unsigned long)(rx_rate % 10U),
                    (unsigned long)(tx_rate / 10U), (unsigned long)(tx_rate % 10U),
                    (unsigned long)gps.pvt_step_ms, (unsigned long)pvt_age,
                    (unsigned long)gps.ubx_errors, (unsigned long)gps.pvt_duplicates,
                    (unsigned long)gps.pvt_gaps, config);
                if (length > 0 && (size_t)length < sizeof(buffer))
                    HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);
                if (pvt_age >= 500U)
                    length = snprintf(buffer, sizeof(buffer), "GPS UBX STALE age=%lu ms\r\n", (unsigned long)pvt_age);
                else
                    length = GPS_UbxFormatPvt(buffer, sizeof(buffer), &gps.pvt);
                if (length > 0 && (size_t)length < sizeof(buffer))
                    HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);
                if (pvt_age < 500U && gps.pvt.fix_ok)
                {
                    length = snprintf(buffer, sizeof(buffer),
                        "GPS VEL N=%ld E=%ld D=%ld mm/s hAcc=%lu vAcc=%lu mm sAcc=%lu mm/s\r\n",
                        (long)gps.pvt.velocity_n_mm_s, (long)gps.pvt.velocity_e_mm_s,
                        (long)gps.pvt.velocity_d_mm_s,
                        (unsigned long)gps.pvt.horizontal_accuracy_mm,
                        (unsigned long)gps.pvt.vertical_accuracy_mm,
                        (unsigned long)gps.pvt.speed_accuracy_mm_s);
                    if (length > 0 && (size_t)length < sizeof(buffer))
                        HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);
                }
                osDelay(1000);
                continue;
            }
            length = snprintf(buffer, sizeof(buffer),
                "GPS CFG=%s Try=%lu ACK=%lu NAK=%lu TxErr=%lu UBXBad=%lu PVT=0 (NMEA fallback)\r\n",
                config, (unsigned long)gps.config_attempts, (unsigned long)gps.config_acks,
                (unsigned long)gps.config_naks, (unsigned long)gps.config_tx_errors,
                (unsigned long)gps.ubx_errors);
            if (length > 0 && (size_t)length < sizeof(buffer))
                HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);
            uint32_t age = now - gps.last_gga_ms;

            if (gps.gga_count == 0U)
                length = snprintf(buffer, sizeof(buffer),
                    "GPS WAIT GGA RX=%lu NMEA=%lu BAD=%lu ERR=%lu\r\n",
                    (unsigned long)gps.rx_bytes,
                    (unsigned long)gps.nmea_lines,
                    (unsigned long)gps.gga_errors,
                    (unsigned long)gps.last_uart_error);
            else if (age >= 3000U)
                length = snprintf(buffer, sizeof(buffer),
                    "GPS STALE age=%lu ms RX=%lu BAD=%lu ERR=%lu\r\n",
                    (unsigned long)age, (unsigned long)gps.rx_bytes,
                    (unsigned long)gps.gga_errors,
                    (unsigned long)gps.last_uart_error);
            else
                length = GPS_NmeaFormatFix(buffer, sizeof(buffer), &gps.fix);

            if (length > 0 && (size_t)length < sizeof(buffer))
                HAL_UART_Transmit(&huart7, (uint8_t *)buffer,
                                  (uint16_t)length, 100);

            if (gps.gsa_count != printed_gsa_count)
            {
                length = snprintf(buffer, sizeof(buffer),
                                  "GPS RAW GSA: %s\r\n", gps.gsa_line);
                if (length > 0 && (size_t)length < sizeof(buffer) &&
                    HAL_UART_Transmit(&huart7, (uint8_t *)buffer,
                                      (uint16_t)length, 100) == HAL_OK)
                    printed_gsa_count = gps.gsa_count;
            }
            if (gps.rmc_count != printed_rmc_count)
            {
                length = snprintf(buffer, sizeof(buffer),
                                  "GPS RAW RMC: %s\r\n", gps.rmc_line);
                if (length > 0 && (size_t)length < sizeof(buffer) &&
                    HAL_UART_Transmit(&huart7, (uint8_t *)buffer,
                                      (uint16_t)length, 100) == HAL_OK)
                    printed_rmc_count = gps.rmc_count;
            }
            if (gps.rmc_decoded_count != 0U)
            {
                char speed[24] = "N/A", course[24] = "N/A";
                bool rmc_fresh = (uint32_t)(now - gps.last_rmc_ms) < 3000U;
                bool gsa_fresh = gps.gsa_decoded_count != 0U &&
                    (uint32_t)(now - gps.last_gsa_ms) < 3000U;
                if (rmc_fresh && gps.rmc.speed_valid)
                    snprintf(speed, sizeof(speed), "%lu.%03lu",
                        (unsigned long)(gps.rmc.speed_mm_s / 1000U),
                        (unsigned long)(gps.rmc.speed_mm_s % 1000U));
                if (rmc_fresh && gps.rmc.course_valid)
                    snprintf(course, sizeof(course), "%lu.%02lu",
                        (unsigned long)(gps.rmc.course_cdeg / 100U),
                        (unsigned long)(gps.rmc.course_cdeg % 100U));
                if (rmc_fresh && gps.rmc.time_valid)
                    length = snprintf(buffer, sizeof(buffer),
                        "GPS NAV FixDim=%u UTC=%04u-%02u-%02u %02lu:%02lu:%02lu Speed=%s m/s Course=%s deg\r\n",
                        gsa_fresh ? (unsigned int)gps.gsa.fix_dimension : 0U,
                        (unsigned int)gps.rmc.year, (unsigned int)gps.rmc.month,
                        (unsigned int)gps.rmc.day,
                        (unsigned long)(gps.rmc.utc_ms / 3600000U),
                        (unsigned long)(gps.rmc.utc_ms / 60000U % 60U),
                        (unsigned long)(gps.rmc.utc_ms / 1000U % 60U), speed, course);
                else
                    length = snprintf(buffer, sizeof(buffer),
                        "GPS NAV time unavailable/stale FixDim=%u Speed=%s m/s Course=%s deg\r\n",
                        gsa_fresh ? (unsigned int)gps.gsa.fix_dimension : 0U, speed, course);
                if (length > 0 && (size_t)length < sizeof(buffer))
                    HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);
            }
        }

        osDelay(1000);
    }
  /* USER CODE END BLUETOOTH */
}

/* USER CODE BEGIN Header_led_blink1 */
/**
* @brief Function implementing the led_blink thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_led_blink1 */
void led_blink1(void *argument)
{
  /* USER CODE BEGIN led_blink1 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(500);
    HAL_GPIO_TogglePin(GPIOI, GPIO_PIN_1);
  }
  /* USER CODE END led_blink1 */
}

/* USER CODE BEGIN Header_StartMavlinkTask */
/**
* @brief Function implementing the mavlinkTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartMavlinkTask */
void StartMavlinkTask(void *argument)
{
  /* USER CODE BEGIN StartMavlinkTask */
  /* Infinite loop */
  MAVLink_Task();
  /* USER CODE END StartMavlinkTask */
}

/* USER CODE BEGIN Header_StartGpsTask */
/**
* @brief Function implementing the gpsTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartGpsTask */
void StartGpsTask(void *argument)
{
  /* USER CODE BEGIN StartGpsTask */
  // Keep reception responsive while the lower-priority Bluetooth task prints.
  osThreadSetPriority(osThreadGetId(), osPriorityNormal);
  GPS_RawTask();
  /* USER CODE END StartGpsTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

