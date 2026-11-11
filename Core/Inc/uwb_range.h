#ifndef UWB_RANGE_H
#define UWB_RANGE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int init_result;          /* 0 ready, 1 starting, negative: probe/idle/init/config */
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
    int16_t clock_offset_raw; /* Relative clock offset: raw / 2^26; last accepted radio frame., debug shit*/
    bool config_verified;
    uint32_t last_update_ms;
    int32_t distance_mm;
    bool valid;               /* latest attempt succeeded; also check sample age */
} UWB_RangeSnapshot;

/* Call once before the scheduler, then run UWB_RangeTask in its own thread.
 * This task is the sole owner of SPI5 and the Qorvo driver. */
void UWB_RangeInit(void);
/* Startup-only: report optional task allocation failure without stopping boot. */
void UWB_RangeTaskUnavailable(void);
void UWB_RangeTask(void *argument);
bool UWB_RangeGetSnapshot(UWB_RangeSnapshot *out);

#endif
