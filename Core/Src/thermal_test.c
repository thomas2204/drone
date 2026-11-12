/* thermal_test.c
 * MLX90640 (shared I2C1 PB8/PB9) -> Bluetooth -> SDMMC1 + FatFs
 *
 * Files written to the card:
 *   SNAP.CSV      one frame as 24 rows x 32 columns of deg C (Notepad / Excel)
 *   THERMAL.AVI   320x240 video: bilinear-interpolated, iron-bow palette.
 *                 Uncompressed AVI; player support varies.
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
#include <stdarg.h>
#include <string.h>

extern I2C_HandleTypeDef hi2c1;

#define MLX_ADDR      0x33
#define REFRESH_RATE  0x04      /* 0x03 = 4 Hz, 0x04 = 8 Hz per subpage.
                                   A full image needs 2 subpages, so the
                                   video frame rate is about half of this. */
#define EMISSIVITY    0.95f
#define TA_SHIFT      8.0f      /* Melexis default for the 55-degree FOV part */
#define VIDEO_FRAMES  60        /* about 15 s at 4 fps */

#define CLIP_SECONDS  30
#define CLIP_PERIOD	  120

/* Sensor calculations use CPU-accessible DTCM. The FIL below uses AXI SRAM. */
static uint16_t       ee[832];
static uint16_t       frameData[834];
static paramsMLX90640 params;
static float          To[768];
static float          Ta;
static FIL fil __attribute__((section(".sd_dma_buffer"), aligned(32)));
static char           line[32 * 8 + 8];
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

/* printf of floats needs extra linker flags, so print x.y by hand */
static const char *f1(float v, char *b)
{
    int t = (int)lroundf(v * 10.0f);
    int a = (t < 0) ? -t : t;
    snprintf(b, 12, "%s%d.%d", (t < 0) ? "-" : "", a / 10, a % 10);
    return b;
}

static int mlx_init(void)
{
    int st;

    if (osMutexAcquire(i2c1MutexHandle, osWaitForever) != osOK) return -1;
    HAL_StatusTypeDef ready = HAL_I2C_IsDeviceReady(&hi2c1, MLX_ADDR << 1, 3, 100);
    osMutexRelease(i2c1MutexHandle);
    if (ready != HAL_OK) {
        printf("MLX90640: no ACK at 0x%02X (check SDA/SCL, power, pull-ups)\r\n", MLX_ADDR);
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
            thermal_report.last_error = sp;
            thermal_report.read_errors++;
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

static void print_stats(void)
{
    float mn = To[0], mx = To[0];
    //char a[12], b[12], c[12], d[12];

    for (int i = 1; i < 768; i++) {
        if (To[i] < mn) mn = To[i];
        if (To[i] > mx) mx = To[i];
    }
    /* center of a 32x24 image is around row 12, column 16 */
    //printf("Ta=%s  min=%s  max=%s  center=%s  (deg C)\r\n",
    //       f1(Ta, a), f1(mn, b), f1(mx, c), f1(To[12 * 32 + 16], d));
}

static void write_snapshot(void)
{
    UINT bw;

    if (AppStorage_Open(&fil, "SNAP.CSV", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("SD: cannot create SNAP.CSV\r\n");
        return;
    }
    for (int r = 0; r < 24; r++) {
        int n = 0;
        for (int c = 0; c < 32; c++) {
            char b[12];
            n += snprintf(line + n, sizeof(line) - n, "%s%c",
                          f1(To[r * 32 + c], b), (c == 31) ? '\n' : ',');
        }
        if (f_write(&fil, line, (UINT)n, &bw) != FR_OK || bw != (UINT)n) {
            printf("SD: write error in SNAP.CSV\r\n");
            break;
        }
    }
    AppStorage_Close(&fil);
    printf("SD: wrote SNAP.CSV\r\n");
}

static void next_name(char *name)
{
	do{
		clip_no++;
		snprintf(name, 16, "T%04lu.AVI",(unsigned long)clip_no);
	}while(f_stat(name, NULL)==FR_OK);
}

static void record_video(void){
	int rc;
    rc = avi_open("THERMAL.AVI");
    if (rc != 0) {
        printf("SD: cannot create THERMAL.AVI (%d)\r\n", rc);
        return;
    }
    printf("SD: recording %d frames, keep the camera pointed at something...\r\n", VIDEO_FRAMES);
    for (int n = 0; n < VIDEO_FRAMES; n++) {
        if (read_frame() < 0) {
            printf("frame read error at %d\r\n", n);
            break;
        }
        rc = avi_add_frame(To);
        if (rc != 0) {
            printf("SD: write error at frame %d (%d)\r\n", n, rc);
            break;
        }
    }
    rc = avi_close();
    printf("SD: THERMAL.AVI closed (%d), %lu frames\r\n", rc, (unsigned long)avi_frames());

}
static void record_clip(uint32_t ms)
{

    char name[16] = {0};
    int bad = 0;

    next_name(name);

    if (avi_open(name) != 0) {
        printf("SD: cannot create %s \r\n",name);
        return;
    }
    //printf("SD: recording %d frames, keep the camera pointed at something...\r\n", VIDEO_FRAMES);
    /*for (int n = 0; n < VIDEO_FRAMES; n++) {
        if (read_frame() < 0) {
            printf("frame read error at %d\r\n", n);
            break;
        }
        rc = avi_add_frame(To);
        if (rc != 0) {
            printf("SD: write error at frame %d (%d)\r\n", n, rc);
            break;
        }
    }
    rc = avi_close();
    printf("SD: THERMAL.AVI closed (%d), %lu frames\r\n", rc, (unsigned long)avi_frames());
    */
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < ms) {
        if (read_frame() < 0) {            /* sensor hiccup: skip it, give up after 10 in a row */
        	if (++bad >= 10) break;
        	continue;
        }
        bad = 0;
        if (avi_add_frame(To) != 0) break; /* SD write error */
    }
    int rc = avi_close();                  /* writes the index and fixes the header */
    thermal_report.storage_error = rc;
    thermal_report.clip_frames = avi_frames();
    if (rc == 0) thermal_report.clips++;
    memcpy((void *)thermal_report.filename, name, sizeof(name));
    printf("SD: %s closed (%d), %lu frames\r\n", name, rc, (unsigned long)avi_frames());
}

void thermal_test_run(void)
{
    FRESULT r;

    printf("\r\n== MLX90640 + TF card test ==\r\n");

    printf("I2C1 state: %u", hi2c1.State);
    printf("SDA level: %d  SCL level: %d",
           HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9),
           HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8));

    while ((thermal_report.init_result = mlx_init()) != 0) {
        osDelay(2000); /* An absent sensor must not spin or stop other tasks. */
    }
    thermal_report.ready = 1;
    printf("MLX90640: OK\r\n");

    for (int i = 0; i < 4; i++) {       /* discard the first frames */
        read_frame();
    }
    print_stats();

    r = AppStorage_Mount();
    printf("SD: mount = %d (0 is OK)\r\n", (int)r);
    if (r == FR_OK) {
        write_snapshot();
        record_video();
    } else {
        printf("SD: skipped (see FRESULT codes in ff.h)\r\n");
    }
    printf("done, now printing live temperatures\r\n");

    for (;;) {
        /*if (read_frame() == 0) {
            print_stats();
        }
        HAL_Delay(250);
        */
        uint32_t t = HAL_GetTick();
        r = AppStorage_Mount();
        if (r == FR_OK) record_clip(CLIP_SECONDS * 1000u);
        else thermal_report.storage_error = r;

        uint32_t used = HAL_GetTick() - t;
        if (used < CLIP_PERIOD * 1000u) {
            osDelay(CLIP_PERIOD * 1000u - used);   /* wait for the next slot */
        }
    }
}
