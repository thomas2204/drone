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
#include "thermal_test.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "mk_app.h"
#include "gps_raw.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include "led_strip.h"
#include "fatfs.h"
#include "audio_player.h"
#include "uwb_range.h"
#include "i2c.h"
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
  .stack_size = 512 * 4,
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
/* Definitions for audio1 */
osThreadId_t audio1Handle;
const osThreadAttr_t audio1_attributes = {
  .name = "audio1",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for thermalTask */
osThreadId_t thermalTaskHandle;
const osThreadAttr_t thermalTask_attributes = {
  .name = "thermalTask",
  .stack_size = 2048 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for i2c1Mutex */
osMutexId_t i2c1MutexHandle;
const osMutexAttr_t i2c1Mutex_attributes = {
  .name = "i2c1Mutex",
  .attr_bits = osMutexPrioInherit,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void BLUETOOTH(void *argument);
void led_blink1(void *argument);
void StartMavlinkTask(void *argument);
void StartGpsTask(void *argument);
void audio(void *argument);
void StartThermalTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
  (void)task;
  (void)name;
  /* No printf or allocation here: the task stack may already be damaged. */
  Error_Handler();
}
/* USER CODE END 4 */

/* USER CODE BEGIN 5 */
void vApplicationMallocFailedHook(void)
{
  Error_Handler();
}
/* USER CODE END 5 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  UWB_RangeInit();
  MAVLink_AppInit();
  GPS_RawInit();
  if (LED_Strip_Init(80) != HAL_OK ||
    LED_Strip_Fill(0, 0, 0, 0) != HAL_OK ||
    LED_Strip_Show() != HAL_OK) {
    Error_Handler();
}
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* CubeMX creates i2c1Mutex immediately after this user section. */
  /* USER CODE END RTOS_MUTEX */

  /* Create the mutex(es) */
  i2c1MutexHandle = osMutexNew(&i2c1Mutex_attributes);

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

  /* creation of audio1 */
  audio1Handle = osThreadNew(audio, NULL, &audio1_attributes);

  /* creation of thermalTask */
  thermalTaskHandle = osThreadNew(StartThermalTask, NULL, &thermalTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  static const osThreadAttr_t uwb_attributes = {
      .name = "uwb_range", .stack_size = 4096, .priority = osPriorityLow
  };
  osThreadId_t uwb_handle = osThreadNew(UWB_RangeTask, NULL, &uwb_attributes);
  /* Optional hardware must not stop GPS/Bluetooth if allocation fails. */
  if (uwb_handle == NULL) UWB_RangeTaskUnavailable();
  (void)uwb_handle;
  /* Fail visibly rather than silently losing a task if allocation fails. */
  configASSERT(defaultTaskHandle != NULL);
  configASSERT(uart_bluetoothHandle != NULL);
  configASSERT(led_blinkHandle != NULL);
  configASSERT(mavlinkTaskHandle != NULL);
  configASSERT(gpsTaskHandle != NULL);
  configASSERT(audio1Handle != NULL);
  configASSERT(thermalTaskHandle != NULL);
  configASSERT(i2c1MutexHandle != NULL);
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
  for (;;)
  {
    osDelay(1000);
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

    for (;;)
    {
        int length;
        if (MAVLink_GetTelemetry(&fc))
        {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            const char *arm = fc.connected
                ? (fc.armed ? "ARMED" : "DISARMED") : "UNKNOWN";
            if (fc.attitude_count == 0U)
                length = snprintf(buffer, sizeof(buffer), "Arm=%s | Attitude unavailable\r\n", arm);
            else if ((uint32_t)(now - fc.last_attitude_ms) >= 1000U)
                length = snprintf(buffer, sizeof(buffer), "Arm=%s | Attitude stale\r\n", arm);
            else
                length = snprintf(buffer, sizeof(buffer), "Yaw=%ld deg Roll=%ld deg Arm=%s\r\n",
                    (long)(fc.yaw * 57.2957795f), (long)(fc.roll * 57.2957795f), arm);
        }
        else
            length = snprintf(buffer, sizeof(buffer), "Telemetry unavailable\r\n");
        if (length > 0 && (size_t)length < sizeof(buffer))
            HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);

        UWB_RangeSnapshot uwb;
        if (!UWB_RangeGetSnapshot(&uwb))
            length = snprintf(buffer, sizeof(buffer), "UWB: unavailable\r\n");
        else if (uwb.init_result > 0)
            length = snprintf(buffer, sizeof(buffer), "UWB: starting\r\n");
        else if (uwb.init_result < 0)
            length = snprintf(buffer, sizeof(buffer), "UWB: unavailable\r\n");
        else if (uwb.valid && (uint32_t)(HAL_GetTick() - uwb.last_update_ms) < 1000U)
        {
            if (uwb.rssi_valid)
                length = snprintf(buffer, sizeof(buffer), "UWB: %ld.%03ld m | Signal: %d dBm\r\n",
                    (long)(uwb.distance_mm / 1000), (long)(uwb.distance_mm % 1000),
                    (int)(uwb.rssi_q8 / 256));
            else
                length = snprintf(buffer, sizeof(buffer), "UWB: %ld.%03ld m\r\n",
                    (long)(uwb.distance_mm / 1000), (long)(uwb.distance_mm % 1000));
        }
        else
            length = snprintf(buffer, sizeof(buffer), "UWB: no range\r\n");
        if (length > 0 && (size_t)length < sizeof(buffer))
            HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);

        if (!GPS_RawGetSnapshot(&gps))
            length = snprintf(buffer, sizeof(buffer), "GPS: unavailable\r\n");
        else if (gps.pvt_count != 0U)
        {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((uint32_t)(now - gps.last_pvt_ms) >= 500U)
                length = snprintf(buffer, sizeof(buffer), "GPS: stale\r\n");
            else
                length = GPS_UbxFormatPvt(buffer, sizeof(buffer), &gps.pvt);
        }
        else if (gps.gga_count == 0U)
            length = snprintf(buffer, sizeof(buffer), "GPS: waiting for data\r\n");
        else
        {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((uint32_t)(now - gps.last_gga_ms) >= 3000U)
                length = snprintf(buffer, sizeof(buffer), "GPS: stale\r\n");
            else
                length = GPS_NmeaFormatFix(buffer, sizeof(buffer), &gps.fix);
        }
        if (length > 0 && (size_t)length < sizeof(buffer))
            HAL_UART_Transmit(&huart7, (uint8_t *)buffer, (uint16_t)length, 100);

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

uint8_t phase = 0;
const uint8_t colours[3][3] = {
    {0, 255, 0},  // Green
    {255, 0, 0},  // Red
    {0, 0, 255}   // Blue
};
for (;;) {
    HAL_GPIO_TogglePin(GPIOI, GPIO_PIN_1);
    osDelay(500);
    if (!LED_Strip_IsBusy()) {
       // LED_Strip_Fill(255, 0, 0, 8);
   
        for (int i = 0; i < 80; i += 5) {
            int c = ((i / 4) + phase) % 3;

            LED_Strip_Set(i,
                          colours[c][0],
                          colours[c][1],
                          colours[c][2],
                          100);
        }

        if (LED_Strip_Show() == HAL_OK) {
            phase = (phase + 1) % 3;
        }
    } 
    LED_Strip_Show();                 // Apply all changes together
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

/* USER CODE BEGIN Header_audio */
/**
* @brief Function implementing the audio1 thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_audio */
void audio(void *argument)
{
  /* USER CODE BEGIN audio */
  FC_Telemetry_t telemetry;
  uint32_t last_mode = UINT32_MAX; /* No mode has been announced yet. */
  AudioPlayer_Play("0:/sound1.wav", 20);
  for (;;)
  {
    
    if (MAVLink_GetTelemetry(&telemetry) &&
        telemetry.heartbeat_count != 0U &&
        telemetry.flight_mode != last_mode)
    {
      /* Remember first, so a missing WAV will not cause repeated attempts. */
      last_mode = telemetry.flight_mode;

      /* ArduPilot Copter mode IDs: 2 = Alt Hold, 5 = Loiter. */
      if (last_mode == 2U)
      {
        AudioPlayer_Play("0:/althold.wav", 100);
      }
      else if (last_mode == 5U)
      {
        AudioPlayer_Play("0:/loiter.wav", 100);
      }
    }
    osDelay(20);
  }
  /* USER CODE END audio */
}

/* USER CODE BEGIN Header_StartThermalTask */
/**
 * @brief Thermal acquisition task; sensor driver has not been supplied yet.
 */
/* USER CODE END Header_StartThermalTask */
void StartThermalTask(void *argument)
{
  /* USER CODE BEGIN StartThermalTask */
  (void)argument;
  /* Integrate camera initialization and acquisition here using hi2c1.
   * Acquire i2c1MutexHandle around each complete I2C operation; release it
   * before temperature calculations, frame waits, logging or display work.
   * There is deliberately no camera traffic until its driver is imported. */
  thermal_test_run();
  
  /* USER CODE END StartThermalTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

