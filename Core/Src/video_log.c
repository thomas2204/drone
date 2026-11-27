/* Camera-owned companion CSV. Snapshot collection never touches SD or radio. */
#include "video_log.h"
#include "camera_avi.h"
#include "uwb_range.h"
#include "gps_raw.h"
#include "app_storage.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t uptime_ms;
    int32_t distance_mm, heading_mdeg, lat_e7, lon_e7, alt_mm;
    int16_t zero_raw;
    uint8_t distance_samples, heading_samples, flags;
} VideoRow;
enum { RANGE=1, HEADING=2, POSITION=4, ALTITUDE=8, CALIBRATED=16 };
static VideoRow rows[CAMERA_AVI_MAX_FRAMES]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
/* Avoid putting the full GPS diagnostic snapshot on the encoder task stack. */
static GPS_RawSnapshot_t gps;
volatile VideoLogReport video_log;
static const char header[] =
    "time_s,video_time_ms,frame,capture_time_ms,uptime_ms,uwb_distance_m,"
    "uwb_heading_deg,gps_lat_deg,gps_lon_deg,gps_alt_msl_m,"
    "uwb_zero_phase_raw,distance_samples,heading_samples\r\n";

void VideoLog_Begin(void)
{
    memset(rows, 0, sizeof rows);
    memset((void *)&video_log, 0, sizeof video_log);
}

void VideoLog_Sample(uint32_t frame)
{
    if (frame >= CAMERA_AVI_MAX_FRAMES) return;
    VideoRow *r = &rows[frame];
    memset(r, 0, sizeof *r);
    UWB_RangeSnapshot uwb;
    bool have_uwb = UWB_RangeGetSnapshot(&uwb);
    bool have_gps = GPS_RawGetSnapshot(&gps);
    /* Read the clock after the snapshots: a new UART/radio epoch may arrive
     * while copying them, and must not appear to be from the future. */
    r->uptime_ms = HAL_GetTick();
    if (have_uwb) {
        r->distance_samples = uwb.distance_samples;
        r->heading_samples = uwb.heading_samples;
        if (uwb.angle_calibrated) { r->flags |= CALIBRATED; r->zero_raw = uwb.zero_phase_raw; }
        if (uwb.init_result == 0 && uwb.valid &&
            (uint32_t)(r->uptime_ms - uwb.last_update_ms) < 500U) {
            if (uwb.distance_filtered_valid) {
                r->flags |= RANGE; r->distance_mm = uwb.distance_filtered_mm;
            }
            if (uwb.angle_valid && uwb.angle_calibrated) {
                r->flags |= HEADING; r->heading_mdeg = uwb.bearing_mdeg;
            }
        }
    }
    if (!have_gps) return;
    if (gps.pvt_count) {
        const GPS_UbxPvt_t *p = &gps.pvt;
        if ((uint32_t)(r->uptime_ms - gps.last_pvt_ms) >= 1500U ||
            !p->fix_ok || (p->fix_type != 2U && p->fix_type != 3U && p->fix_type != 4U)) return;
        r->flags |= POSITION;
        r->lat_e7 = p->latitude_e7; r->lon_e7 = p->longitude_e7;
        if (p->fix_type != 2U) { r->flags |= ALTITUDE; r->alt_mm = p->altitude_msl_mm; }
    } else if (gps.gga_count && gps.fix.position_valid && gps.fix.fix_quality &&
               (uint32_t)(r->uptime_ms - gps.last_gga_ms) < 1500U) {
        r->flags |= POSITION;
        r->lat_e7 = gps.fix.latitude_e7; r->lon_e7 = gps.fix.longitude_e7;
        if (gps.fix.altitude_valid && gps.gsa.fix_dimension >= 3U &&
            gps.gsa_decoded_count && (uint32_t)(r->uptime_ms - gps.last_gsa_ms) < 1500U) {
            r->flags |= ALTITUDE; r->alt_mm = gps.fix.altitude_msl_mm;
        }
    }
}

bool VideoLog_Name(char out[20], const char *video)
{
    size_t n = strlen(video);
    if (n < 4U || n >= 20U || video[n - 4U] != '.') return false;
    memcpy(out, video, n - 3U);
    memcpy(out + n - 3U, "CSV", 4);
    return true;
}

static void fixed(char out[24], int32_t value, unsigned digits)
{
    uint32_t scale = 1;
    for (unsigned i = 0; i < digits; ++i) scale *= 10U;
    uint32_t magnitude = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    snprintf(out, 24, "%s%lu.%0*lu", value < 0 ? "-" : "",
        (unsigned long)(magnitude / scale), (int)digits, (unsigned long)(magnitude % scale));
}

static int format_row(char *out, size_t size, const VideoRow *r, uint32_t frame,
                      uint32_t frames, uint32_t duration, uint32_t start)
{
    uint32_t time = (uint32_t)((uint64_t)frame * duration / frames);
    int32_t elapsed = (int32_t)(r->uptime_ms - start);
    char distance[24]="NA", heading[24]="NA", lat[24]="NA", lon[24]="NA", alt[24]="NA", zero[16]="NA";
    if (r->flags & RANGE) fixed(distance, r->distance_mm, 3);
    if (r->flags & HEADING) fixed(heading, r->heading_mdeg, 3);
    if (r->flags & POSITION) { fixed(lat, r->lat_e7, 7); fixed(lon, r->lon_e7, 7); }
    if (r->flags & ALTITUDE) fixed(alt, r->alt_mm, 3);
    if (r->flags & CALIBRATED) snprintf(zero, sizeof zero, "%d", (int)r->zero_raw);
    return snprintf(out, size, "%lu.%03lu,%lu,%lu,%lu,%lu,%s,%s,%s,%s,%s,%s,%u,%u\r\n",
        (unsigned long)(time/1000U), (unsigned long)(time%1000U), (unsigned long)time,
        (unsigned long)frame, (unsigned long)(elapsed < 0 ? 0 : elapsed),
        (unsigned long)r->uptime_ms, distance, heading, lat, lon, alt, zero,
        (unsigned)r->distance_samples, (unsigned)r->heading_samples);
}

static uint32_t hash_bytes(uint32_t hash, const uint8_t *data, uint32_t size)
{
    while (size--) { hash ^= *data++; hash *= 16777619U; }
    return hash;
}

FRESULT VideoLog_Write(FIL *file, const char *video, uint32_t frames,
                      uint32_t duration, uint32_t start, uint8_t *scratch, uint32_t capacity)
{
    char name[20], line[224];
    if (!frames || frames > CAMERA_AVI_MAX_FRAMES || !duration || capacity < sizeof header ||
        !VideoLog_Name(name, video)) return FR_INVALID_PARAMETER;
    FRESULT result = AppStorage_Open(file, name, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK) { video_log.result = result; return result; }
    memcpy((void *)video_log.filename, name, strlen(name) + 1U);
    uint32_t used = sizeof header - 1U;
    uint32_t expected_hash = 2166136261U;
    video_log.bytes = video_log.verified_bytes = 0;
    memcpy(scratch, header, used);
    for (uint32_t i = 0; i <= frames; ++i) {
        int n = i < frames ? format_row(line, sizeof line, &rows[i], i, frames, duration, start) : 0;
        if (n < 0 || (size_t)n >= sizeof line || (uint32_t)n > capacity) { result = FR_INT_ERR; break; }
        if (used && (i == frames || used + (uint32_t)n > capacity)) {
            UINT written = 0;
            result = f_write(file, scratch, (UINT)used, &written);
            if (result != FR_OK || written != used) { if (result == FR_OK) result = FR_DISK_ERR; break; }
            expected_hash = hash_bytes(expected_hash, scratch, used);
            video_log.bytes += used;
            used = 0;
        }
        if (i < frames) { memcpy(scratch + used, line, (size_t)n); used += (uint32_t)n; }
    }
    if (result == FR_OK) result = f_sync(file);
    FRESULT closed = AppStorage_Close(file);
    if (result == FR_OK) result = closed;
    if (result == FR_OK) {
        result = AppStorage_Open(file, name, FA_READ);
        if (result == FR_OK) {
            uint32_t observed_hash = 2166136261U;
            if (f_size(file) != video_log.bytes) result = FR_INT_ERR;
            while (result == FR_OK && video_log.verified_bytes < video_log.bytes) {
                uint32_t n = video_log.bytes - video_log.verified_bytes;
                if (n > capacity) n = capacity;
                UINT received = 0;
                result = f_read(file, scratch, (UINT)n, &received);
                if (result != FR_OK || received != n) { if (result == FR_OK) result = FR_DISK_ERR; break; }
                observed_hash = hash_bytes(observed_hash, scratch, n);
                video_log.verified_bytes += n;
            }
            if (result == FR_OK && observed_hash != expected_hash) result = FR_INT_ERR;
            closed = AppStorage_Close(file);
            if (result == FR_OK) result = closed;
        }
    }
    video_log.result = result;
    if (result == FR_OK) video_log.rows = frames;
    return result;
}
