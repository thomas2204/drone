#ifndef UWB_RANGE_H
#define UWB_RANGE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int init_result;          /* 0 ready, positive starting; -6: not PDoA silicon. */
    uint32_t device_id;
    uint32_t polls, received, timeouts, errors, rejected;
    uint32_t transmitted, frames;
    int16_t rssi_q8;          /* Most recent received channel power, Q8.8 dBm. */
    bool rssi_valid;
    uint8_t rf_port;
    uint32_t tx_power_readback, chan_readback, rf_switch_readback;
    uint16_t preamble_readback, sfd_timeout_readback;
    uint8_t bitrate_readback, pac_readback;
    uint32_t cir_power;
    uint16_t accumulation_count;
    uint8_t dgc_decision;
    int16_t clock_offset_raw; /* Relative clock offset: raw / 2^26. */
    uint32_t sys_cfg_readback;
    uint8_t sts_length_readback;
    int16_t pdoa_raw;          /* Phase difference in radians * 2048, NOT bearing. */
    int16_t sts_quality;
    uint16_t sts_status[2];
    int64_t tdoa_raw;
    bool pdoa_valid;           /* Valid phase from the latest accepted range. */
    int16_t pdoa_filtered_raw;
    bool pdoa_filtered_valid;
    bool angle_calibrated;
    bool angle_valid;
    int32_t bearing_mdeg;      /* Ideal calibrated front-sector bearing. */
    uint32_t pdoa_rejected;
    bool config_verified;
    uint32_t last_update_ms;
    int32_t distance_mm;
    bool valid;               /* latest attempt succeeded; also check sample age */
    int32_t distance_filtered_mm;
    bool distance_filtered_valid;
    uint8_t distance_samples, heading_samples;
    int16_t zero_phase_raw;    /* Saved/applied forward-zero phase, radians * 2048. */
} UWB_RangeSnapshot;

/* Call once before the scheduler, then run UWB_RangeTask in its own thread.
 * This task is the sole owner of SPI5 and the Qorvo driver. */
void UWB_RangeInit(void);
/* Startup-only: report optional task allocation failure without stopping boot. */
void UWB_RangeTaskUnavailable(void);
void UWB_RangeTask(void *argument);
bool UWB_RangeGetSnapshot(UWB_RangeSnapshot *out);
/* Bluetooth command task: capture the current fresh filtered phase as straight
 * ahead. The UWB task applies it and reports the SD save result. */
bool UWB_RangeZeroBearing(void);

#endif
