#ifndef PMW3901_H
#define PMW3901_H

#include <stdbool.h>
#include <stdint.h>

/* Nominal angular scale used by ArduPilot's native PixArt backend. */
#define PMW3901_RADIANS_PER_COUNT 0.00126f
#define PMW3901_MAX_SAMPLE_MS 60U

typedef struct {
    uint32_t sequence;
    uint32_t capture_ms;
    uint32_t interval_ms;
    uint32_t spi_errors;
    int16_t dx, dy;
#if PMW3901_BENCH_DIAGNOSTICS
    uint16_t shutter;
    uint8_t motion, observation;
#endif
    uint8_t quality;
    uint8_t product_id, inverse_id;
    bool ready;
    bool valid;
} PMW3901Reading;

void PMW3901_Task(void *argument);
void PMW3901_Read(PMW3901Reading *reading);

#endif
