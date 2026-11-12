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
#include "led_strip.h"
#include "fatfs.h"
#include "audio_player.h"
#include "uwb_range.h"
#include "i2c.h"
#include "app_storage.h"
#include "app_log.h"
#if APP_CAMERA_ENABLED
#include "camera_snapshot.h"
#endif
#if BOARD_BENCH_TEST
#include "board_bench.h"
#endif
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
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for thermalTask */
osThreadId_t thermalTaskHandle;
const osThreadAttr_t thermalTask_attributes = {
  .name = "thermalTask",
  .stack_size = 2048 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
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
  if (!AppStorage_Init() || !AppLog_Init()) Error_Handler();
#if BOARD_CAMERA_TEST
  static const osThreadAttr_t camera_attributes = {
      .name = "camera_test", .stack_size = 16384, .priority = osPriorityNormal
  };
  i2c1MutexHandle = osMutexNew(&i2c1Mutex_attributes);
  if (i2c1MutexHandle == NULL ||
      osThreadNew(CameraSnapshot_Task, NULL, &camera_attributes) == NULL)
    Error_Handler();
  return;
#endif
#if BOARD_BENCH_TEST
  static const osThreadAttr_t bench_attributes = {
      .name = "board_bench", .stack_size = 4096, .priority = osPriorityNormal
  };
  if (osThreadNew(BoardBench_Task, NULL, &bench_attributes) == NULL)
    Error_Handler();
  return;
#endif
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
#if APP_CAMERA_ENABLED
  static const osThreadAttr_t recorder_attributes = {
      .name="camera_record", .stack_size=16384, .priority=osPriorityLow
  };
  configASSERT(osThreadNew(CameraSnapshot_Task,NULL,&recorder_attributes)!=NULL);
#endif
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
#if GPS_DIAGNOSTICS_ENABLED
    uint32_t last_diagnostic_ms = 0;
    uint32_t previous_pvt_count = 0;
#endif

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
            AppLog_Write(buffer, (uint16_t)length, 100);

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
            AppLog_Write(buffer, (uint16_t)length, 100);

#if APP_CAMERA_ENABLED
        length = CameraSnapshot_FormatStatus(buffer, sizeof(buffer));
        if (length > 0 && (size_t)length < sizeof(buffer))
            AppLog_Write(buffer, (uint16_t)length, 100);
#endif

        bool gps_available = GPS_RawGetSnapshot(&gps);
        if (!gps_available)
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
            AppLog_Write(buffer, (uint16_t)length, 100);

#if GPS_DIAGNOSTICS_ENABLED
        uint32_t diagnostic_now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (gps_available && (uint32_t)(diagnostic_now - last_diagnostic_ms) >= 10000U)
        {
            static const char *config_names[] = {"WAIT", "SENT", "ACK", "NAK", "TIMEOUT"};
            uint32_t elapsed = diagnostic_now - last_diagnostic_ms;
            uint32_t rate_x10 = (uint32_t)((uint64_t)(gps.pvt_count - previous_pvt_count) * 10000U / elapsed);
            previous_pvt_count = gps.pvt_count;
            last_diagnostic_ms = diagnostic_now;
            length = snprintf(buffer, sizeof(buffer),
                "GPS SETUP CFG=%s attempts=%lu ack=%lu nak=%lu txerr=%lu BDS-test=%u targetStep=%ums\r\n",
                (unsigned)gps.config_state < 5U ? config_names[gps.config_state] : "?",
                (unsigned long)gps.config_attempts,(unsigned long)gps.config_acks,
                (unsigned long)gps.config_naks,(unsigned long)gps.config_tx_errors,(unsigned)GPS_BDS_TEST_PROFILE,
                (unsigned)GPS_MEASUREMENT_PERIOD_MS);
            if (length > 0 && (size_t)length < sizeof(buffer))
                AppLog_Write(buffer, (uint16_t)length, 100);
            length = snprintf(buffer, sizeof(buffer),
                "GPS PVT count=%lu RX=%lu.%luHz step=%lums age=%lums bad=%lu dup=%lu gap=%lu\r\n",
                (unsigned long)gps.pvt_count,(unsigned long)(rate_x10/10U),(unsigned long)(rate_x10%10U),
                (unsigned long)gps.pvt_step_ms,(unsigned long)(diagnostic_now-gps.last_pvt_ms),
                (unsigned long)gps.ubx_errors,(unsigned long)gps.pvt_duplicates,(unsigned long)gps.pvt_gaps);
            if (length > 0 && (size_t)length < sizeof(buffer))
                AppLog_Write(buffer, (uint16_t)length, 100);
            length = snprintf(buffer, sizeof(buffer),
                "GPS ACC valid=%u type=%u flags=0x%02x hAcc=%lumm vAcc=%lumm sAcc=%lumm/s HDOPx100=%u VDOPx100=%u sameEpoch=%u\r\n",
                (unsigned)gps.pvt.fix_ok,(unsigned)gps.pvt.fix_type,(unsigned)gps.pvt.flags,
                (unsigned long)gps.pvt.horizontal_accuracy_mm,(unsigned long)gps.pvt.vertical_accuracy_mm,
                (unsigned long)gps.pvt.speed_accuracy_mm_s,(unsigned)gps.dop.hdop_x100,(unsigned)gps.dop.vdop_x100,
                (unsigned)(gps.dop_count != 0U && gps.pvt_count != 0U && gps.dop.tow_ms == gps.pvt.tow_ms));
            if (length > 0 && (size_t)length < sizeof(buffer))
                AppLog_Write(buffer, (uint16_t)length, 100);
            for (unsigned line = 0; line < GPS_DIAG_BASE_LINES + GPS_DIAG_TOP_SATS; ++line)
            {
                length = GPS_DiagFormatLine(&gps.diagnostic, line, diagnostic_now, buffer, sizeof(buffer));
                if (length == 0) break;
                if (length > 0 && (size_t)length < sizeof(buffer))
                    AppLog_Write(buffer, (uint16_t)length, 100);
            }
        }
#endif
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
  for (;;)
  {
    osDelay(1000);
  }
  /* USER CODE END StartThermalTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

