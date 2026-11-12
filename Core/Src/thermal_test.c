/* thermal_test.c
 * MLX90640 (shared I2C1 PB8/PB9) -> Bluetooth -> SDMMC1 + FatFs
 *
 * Bluetooth "record": one numbered Txxxx.AVI, 10-second shared capture
 * window with OV2640. Bilinear 320x240, iron-bow palette. Player support varies.
 */
#include "thermal_test.h"
#include "i2c.h"
#include "thermal_avi.h"
#include "main.h"
#include "fatfs.h"          /* SDPath */
#include "MLX90640_API.h"
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "cmsis_os.h"
#include "app_storage.h"
#include "app_log.h"
#include "record_control.h"
#include <stdarg.h>
#include <string.h>

extern I2C_HandleTypeDef hi2c1;

#define MLX_ADDR      0x33
#define REFRESH_RATE  0x04      /* 0x03 = 4 Hz, 0x04 = 8 Hz per subpage.
                                   A full image needs 2 subpages, so the
                                   video frame rate is about half of this. */
#define EMISSIVITY    0.95f
#define TA_SHIFT      8.0f      /* Melexis default for the 55-degree FOV part */


/* Sensor calculations use CPU-accessible DTCM. AVI file I/O uses AXI SRAM. */
static uint16_t       ee[832];
static uint16_t       frameData[834];
static paramsMLX90640 params;
static float          To[768];
static float          Ta;
static uint32_t	 	  clip_no;
volatile ThermalReport thermal_report;

static int ThermalLog(const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length > 0) AppLog_Write(message,
        (uint16_t)((size_t)length < sizeof(message) ? length : sizeof(message)-1U), 100);
    return length;
}
#define printf ThermalLog

/* Called only during thermal startup while the shared bus mutex is held.
 * A failed BUSY/timeout probe can leave the peripheral driving a line low.
 * Release both pins briefly with weak pull-ups, then restore every GPIO bit
 * and CR1 exactly. This neither drives push-pull nor changes clock/timing. */
static bool release_bus_locked(void)
{
    uint32_t control=hi2c1.Instance->CR1;
    uint32_t mode=GPIOB->MODER, pull=GPIOB->PUPDR;
    __HAL_I2C_DISABLE(&hi2c1);
    GPIOB->MODER=mode & ~(0xFU << 16);
    GPIOB->PUPDR=(pull & ~(0xFU << 16)) | (0x5U << 16);
    osDelay(2);
    uint32_t lines=(GPIOB->IDR >> 8) & 3U;
    GPIOB->PUPDR=pull;
    GPIOB->MODER=mode;
    hi2c1.Instance->CR1=control;
    thermal_report.probe_recoveries++;
    thermal_report.probe_recovery_lines=lines;
    return lines==3U;
}

static int mlx_init(void)
{
    int st;

    if (osMutexAcquire(i2c1MutexHandle, osWaitForever) != osOK) return -1;
    HAL_StatusTypeDef ready = HAL_I2C_IsDeviceReady(&hi2c1, MLX_ADDR << 1, 3, 100);
    if (ready==HAL_BUSY || (HAL_I2C_GetError(&hi2c1) & HAL_I2C_ERROR_TIMEOUT)) {
        thermal_report.probe_previous_error=HAL_I2C_GetError(&hi2c1);
        if (release_bus_locked())
            ready=HAL_I2C_IsDeviceReady(&hi2c1, MLX_ADDR << 1, 3, 100);
    }
    thermal_report.probe_attempts++;
    thermal_report.probe_hal=ready;
    thermal_report.probe_i2c_error=HAL_I2C_GetError(&hi2c1);
    thermal_report.probe_isr=hi2c1.Instance->ISR;
    thermal_report.probe_scl=HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_8);
    thermal_report.probe_sda=HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_9);
    osMutexRelease(i2c1MutexHandle);
    if (ready != HAL_OK) {
        printf("MLX90640: probe 0x%02X failed HAL=%lu I2C=0x%lX ISR=0x%lX SCL=%lu SDA=%lu\r\n",
            MLX_ADDR,(unsigned long)thermal_report.probe_hal,
            (unsigned long)thermal_report.probe_i2c_error,(unsigned long)thermal_report.probe_isr,
            (unsigned long)thermal_report.probe_scl,(unsigned long)thermal_report.probe_sda);
        return -1;
    }
    st = MLX90640_DumpEE(MLX_ADDR, ee);
    if (st != 0) {
        printf("MLX90640: DumpEE failed (%d)\r\n", st);
        return -2;
    }
    st = MLX90640_ExtractParameters(ee, &params);
    if (st != 0) {
        printf("MLX90640: ExtractParameters failed (%d)\r\n", st);
        return -3;
    }
    return MLX90640_SetRefreshRate(MLX_ADDR, REFRESH_RATE);
}

/* Keeps reading until BOTH subpages have been seen, so To[] is a complete,
 * up-to-date image even if a subpage was missed while we were busy. */
static int read_frame(void)
{
    uint8_t seen = 0;

    for (int tries = 0; tries < 8 && seen != 3; tries++) {
        int sp = MLX90640_GetFrameData(MLX_ADDR, frameData);  /* subpage 0/1, <0 = error */
        if (sp < 0) {
            if (!(record_report.started && !RecordControl_HasTime())) {
                thermal_report.last_error = sp;
                thermal_report.read_errors++;
            }
            return sp;
        }
        seen |= (uint8_t)(1u << (sp & 1));
        Ta = MLX90640_GetTa(frameData, &params);
        MLX90640_CalculateTo(frameData, &params, EMISSIVITY, Ta - TA_SHIFT, To);
    }
    if (seen != 3) return -100;
    thermal_report.frames++;
    thermal_report.ambient_c = Ta;
    thermal_report.center_c = To[12 * 32 + 16];
    return 0;
}

static void next_name(char *name)
{
    do {
        clip_no++;
        snprintf(name,16,"T%04lu.AVI",(unsigned long)clip_no);
    } while (f_stat(name,NULL)==FR_OK);
}

static bool record_clip(void)
{
    char name[16]={0};
    FRESULT mounted=AppStorage_Mount();
    if (mounted!=FR_OK) { thermal_report.storage_error=mounted; return false; }
    next_name(name);
    int rc=avi_open(name);
    if (rc) { thermal_report.storage_error=rc; return false; }
    bool success=RecordControl_WaitStart(RECORD_THERMAL);
    thermal_report.storage_error=0;
    if (success) {
        while (RecordControl_HasTime()) {
            int error=read_frame();
            if (!RecordControl_HasTime()) break;
            if (error<0) { success=false; break; }
            rc=avi_add_frame(To);
            if (rc) { thermal_report.storage_error=rc; success=false; break; }
        }
    }
    if (record_report.failed) success=false;
    rc=avi_close_duration(record_report.started ? record_report.duration_ms : 0);
    if (rc) { thermal_report.storage_error=rc; success=false; }
    thermal_report.clip_frames=avi_frames();
    thermal_report.duration_ms=record_report.started ? record_report.duration_ms : 0;
    if (!thermal_report.clip_frames) success=false;
    if (success) thermal_report.clips++;
    memcpy((void *)thermal_report.filename,name,sizeof(name));
    printf("THERMAL: %s %s frames=%lu duration=%lums\r\n",success?"saved":"failed",
        name,(unsigned long)thermal_report.clip_frames,(unsigned long)thermal_report.duration_ms);
    return success;
}

void thermal_test_run(void)
{
    printf("THERMAL: initializing MLX90640 on shared I2C1\r\n");
    while ((thermal_report.init_result=mlx_init())!=0) osDelay(2000);
    for (int i=0;i<4;i++) read_frame();
    thermal_report.ready=1;
    printf("THERMAL: ready; send record for a 10-second clip\r\n");
    for (;;) {
        if (RecordControl_TakeRequest(RECORD_THERMAL)) {
            bool success=record_clip();
            RecordControl_Done(RECORD_THERMAL,success);
        }
        osDelay(20);
    }
}
