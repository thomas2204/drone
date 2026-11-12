#ifndef CAMERA_SNAPSHOT_H
#define CAMERA_SNAPSHOT_H
#include <stdint.h>
#include <stddef.h>
typedef enum {
    CAMERA_STARTING, CAMERA_SENSOR, CAMERA_CONFIGURE, CAMERA_CAPTURE,
    CAMERA_VALIDATE, CAMERA_SD_INIT, CAMERA_MOUNT, CAMERA_WRITE,
    CAMERA_VERIFY, CAMERA_DONE
} CameraStage;
typedef enum {
    CAMERA_OK, CAMERA_I2C, CAMERA_SENSOR_ID, CAMERA_DMA, CAMERA_TIMEOUT,
    CAMERA_OVERFLOW, CAMERA_JPEG, CAMERA_SD, CAMERA_FS, CAMERA_DATA,
    CAMERA_CACHE_ENABLED, CAMERA_REGISTER_MISMATCH
} CameraFailure;
typedef struct {
    uint32_t stage, failure, done;
    uint32_t sensor_id, i2c_error, failed_reg, hal_status;
    uint32_t dcmi_error, dma_error, frames, vsyncs, captured_bytes;
    uint32_t jpeg_offset, jpeg_bytes, width, height;
    uint32_t sd_error, fatfs_result, close_result, unmount_result;
    uint32_t bytes_written, bytes_verified;
    uint32_t sd_requested;
    uint32_t software_sccb, sccb_nacks;
    uint32_t skip_dsp_prefix;
    uint32_t pclk_edges[2], href_edges[2], vsync_edges[2];
    char filename[20];
    uint32_t probe_registers, bank_readback[3], trace_count;
    /* First 32 SCCB transfers: register/value, read flag, HAL result/error,
     * and sampled PB8/PB9 levels after the transfer. */
    uint32_t trace[32][6];
    uint32_t register_checks, register_values[2][4];
    uint32_t output_registers[12], output_activity[6][3];
    uint32_t video_requested, video_frames, video_duration_ms;
    uint32_t dma_memory_pass[2], raw_trials[8][9];
    uint32_t dcmi_selftest_bytes, dcmi_selftest_word, dcmi_selftest_matches;
    uint32_t selftest_gpio_data, selftest_gpio_a, selftest_gpio_h;
    uint32_t video_uncompressed;
    uint32_t video_mp4;
} CameraSnapshotReport;
extern volatile CameraSnapshotReport camera_snapshot;
/* Set to 1 to request another photo after completion or failure. */
extern volatile uint32_t camera_snapshot_request;
/* Diagnostic: set to zero before a request to capture without touching SD. */
extern volatile uint32_t camera_snapshot_save_to_sd;
extern volatile uint32_t camera_snapshot_software_sccb;
/* Diagnostic only: omit the three-entry reserved DSP preamble. */
extern volatile uint32_t camera_snapshot_skip_dsp_prefix;
/* 1: bank readbacks; 2: reversible register test; 3: exact user table;
 * 4: user table excluding first 3 entries; 5: documented output/clock probe.
 * Modes 2..5 skip capture and SD; 6 records a labelled synthetic SD test clip;
 * 7 tests DMA access and raw RGB capture across all sync/clock polarities;
 * 8 injects a weak-pull waveform with AF13 selected and camera powered down. */
extern volatile uint32_t camera_snapshot_probe_registers;
/* Camera build defaults to 40-frame MJPEG AVI, up to 4 fps, max 120 frames.
 * Set zero to save a single JPEG instead. */
extern volatile uint32_t camera_snapshot_video_frames;
/* 0xFFFFFFFF preserves profile CLKRC; 0x80 enables the documented doubler. */
extern volatile uint32_t camera_snapshot_clockrc;
extern volatile uint32_t camera_snapshot_selftest_pattern;
/* Diagnostic polarity bits: 0 falling PCLK, 1 high HSYNC, 2 low VSYNC. */
extern volatile uint32_t camera_snapshot_dcmi_polarities;
/* Default 1: Windows RGB24 uncompressed AVI; 0 keeps smaller MJPEG AVI. */
extern volatile uint32_t camera_snapshot_video_uncompressed;
/* Default 1: encode H.264 Baseline and write MP4 directly on the H743. */
extern volatile uint32_t camera_snapshot_video_mp4;
/* Debug export: reread a completed file into capture if it fits 128KiB. */
extern volatile uint32_t camera_snapshot_export_recording;
void CameraSnapshot_Task(void *argument);
int CameraSnapshot_FormatStatus(char *buffer, size_t size);
#endif
