/* CSV logger for UWB distance and bearing. One row per measurement attempt
 * (about 10 Hz). Distance/bearing cells are left empty when that measurement
 * was not valid, so failed attempts are visible but never show stale numbers. */
#include "uwb_log.h"
#include "uwb_range.h"
#include "app_storage.h"
#include "ff.h"
#include "main.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

#define UWB_LOG_FILE      "UWB.CSV"
#define UWB_LOG_SYNC_MS   1000U   /* flush to the card once per second */
#define UWB_LOG_POLL_MS   20U     /* check for a new measurement */
#define UWB_LOG_RETRY_MS  2000U   /* wait after an SD error */

/* FIL holds a 512-byte sector buffer that the SDMMC DMA uses, so it must live
 * in the same DMA-reachable section as the other log files (see LD6002_UART.c). */
static FIL uwb_fil __attribute__((section(".sd_dma_buffer"), aligned(32)));

static volatile bool log_enabled = UWB_LOG_AUTOSTART;

/* Debugger-visible diagnostics. */
volatile uint32_t uwb_log_rows;
volatile uint32_t uwb_log_open_errors;
volatile uint32_t uwb_log_write_errors;

static const char header[] =
    "time_ms,valid,distance_mm,distance_m,angle_valid,bearing_mdeg,bearing_deg,"
    "angle_calibrated,pdoa_raw,pdoa_filtered_raw,rssi_dbm,sts_quality,"
    "polls,received,timeouts,errors,rejected\r\n";

void UWB_LogSetEnabled(bool enabled) { log_enabled = enabled; }
bool UWB_LogIsEnabled(void) { return log_enabled; }

/* Integer-only "-1.234" formatting: the embedded printf has no float support. */
static void format_milli(char *out, size_t capacity, int32_t value)
{
    uint32_t magnitude = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    snprintf(out, capacity, "%s%lu.%03lu", value < 0 ? "-" : "",
             (unsigned long)(magnitude / 1000U), (unsigned long)(magnitude % 1000U));
}

static int build_row(char *line, size_t capacity, uint32_t now, const UWB_RangeSnapshot *s)
{
    char distance_mm[16] = "", distance_m[20] = "";
    char bearing_mdeg[16] = "", bearing_deg[20] = "";
    char pdoa[12] = "", pdoa_filtered[12] = "", rssi[12] = "", quality[12] = "";

    if (s->valid) {
        snprintf(distance_mm, sizeof distance_mm, "%ld", (long)s->distance_mm);
        format_milli(distance_m, sizeof distance_m, s->distance_mm);
        snprintf(quality, sizeof quality, "%d", (int)s->sts_quality);
    }
    if (s->angle_valid) {
        snprintf(bearing_mdeg, sizeof bearing_mdeg, "%ld", (long)s->bearing_mdeg);
        format_milli(bearing_deg, sizeof bearing_deg, s->bearing_mdeg);
    }
    if (s->pdoa_valid)
        snprintf(pdoa, sizeof pdoa, "%d", (int)s->pdoa_raw);
    if (s->pdoa_filtered_valid)
        snprintf(pdoa_filtered, sizeof pdoa_filtered, "%d", (int)s->pdoa_filtered_raw);
    if (s->rssi_valid)
        snprintf(rssi, sizeof rssi, "%d", (int)(s->rssi_q8 / 256));

    return snprintf(line, capacity,
        "%lu,%d,%s,%s,%d,%s,%s,%d,%s,%s,%s,%s,%lu,%lu,%lu,%lu,%lu\r\n",
        (unsigned long)now, s->valid ? 1 : 0, distance_mm, distance_m,
        s->angle_valid ? 1 : 0, bearing_mdeg, bearing_deg,
        s->angle_calibrated ? 1 : 0, pdoa, pdoa_filtered, rssi, quality,
        (unsigned long)s->polls, (unsigned long)s->received,
        (unsigned long)s->timeouts, (unsigned long)s->errors,
        (unsigned long)s->rejected);
}

static bool write_text(const char *text, UINT length)
{
    UINT written = 0;
    FRESULT result = f_write(&uwb_fil, text, length, &written);
    return result == FR_OK && written == length;
}

void UWB_LogTask(void *argument)
{
    (void)argument;
    bool open = false;
    uint32_t last_polls = UINT32_MAX;
    uint32_t last_sync = 0;
    char line[192];

    for (;;) {
        if (!log_enabled) {
            if (open) { (void)AppStorage_Close(&uwb_fil); open = false; }
            osDelay(100);
            continue;
        }

        if (!open) {
            if (AppStorage_Open(&uwb_fil, UWB_LOG_FILE, FA_OPEN_APPEND | FA_WRITE) != FR_OK) {
                ++uwb_log_open_errors;
                osDelay(UWB_LOG_RETRY_MS);
                continue;
            }
            open = true;
            /* Captions go in once, when the file is new or empty. */
            if (f_size(&uwb_fil) == 0U &&
                !write_text(header, (UINT)(sizeof header - 1U))) {
                ++uwb_log_write_errors;
                (void)AppStorage_Close(&uwb_fil); open = false;
                osDelay(UWB_LOG_RETRY_MS);
                continue;
            }
            (void)f_sync(&uwb_fil);
            last_sync = HAL_GetTick();
        }

        UWB_RangeSnapshot snapshot;
        if (UWB_RangeGetSnapshot(&snapshot) && snapshot.init_result == 0 &&
            snapshot.polls != last_polls) {
            last_polls = snapshot.polls;
            int length = build_row(line, sizeof line, HAL_GetTick(), &snapshot);
            if (length > 0 && (size_t)length < sizeof line &&
                write_text(line, (UINT)length)) {
                ++uwb_log_rows;
            } else {
                ++uwb_log_write_errors;
                (void)AppStorage_Close(&uwb_fil); open = false;
                osDelay(UWB_LOG_RETRY_MS);
                continue;
            }
        }

        if (open && (uint32_t)(HAL_GetTick() - last_sync) >= UWB_LOG_SYNC_MS) {
            if (f_sync(&uwb_fil) != FR_OK) ++uwb_log_write_errors;
            last_sync = HAL_GetTick();
        }
        osDelay(UWB_LOG_POLL_MS);
    }
}
