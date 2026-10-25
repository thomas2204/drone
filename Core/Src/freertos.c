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
    GPS_RawSnapshot_t gps = {0};

    for (;;)
    {
        int length = 0;

        if (MAVLink_GetTelemetry(&fc))
        {
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

        if (GPS_RawGetSnapshot(&gps))
        {
            uint32_t now = (uint32_t)(xTaskGetTickCount()
                                      * portTICK_PERIOD_MS);
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

