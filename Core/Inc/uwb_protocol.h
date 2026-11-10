#ifndef UWB_PROTOCOL_H
#define UWB_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CRC is checked by the radio. Times are DW3000 low-32-bit device ticks. */
bool UWB_DecodeRange(const uint8_t *frame, size_t length, uint8_t sequence,
                     uint32_t poll_tx, uint32_t response_rx,
                     int16_t clock_offset, int32_t *distance_mm);
#endif
