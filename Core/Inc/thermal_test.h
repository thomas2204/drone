#ifndef THERMAL_TEST_H
#define THERMAL_TEST_H
#include <stdint.h>

typedef struct {
    int32_t init_result, last_error, storage_error;
    uint32_t ready, frames, read_errors, clips, clip_frames, duration_ms;
    float ambient_c, center_c;
    char filename[16];
    uint32_t probe_attempts, probe_hal, probe_i2c_error, probe_isr, probe_scl, probe_sda;
    uint32_t probe_recoveries, probe_previous_error, probe_recovery_lines;
} ThermalReport;
extern volatile ThermalReport thermal_report;

/* Reads the MLX90640, prints temperatures over the serial port, and logs
 * frames to the TF card. Never returns. Call from the thermal RTOS task. */
void thermal_test_run(void);

#endif
