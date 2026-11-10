#include "uwb_protocol.h"
#include <math.h>
#include <string.h>

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool UWB_DecodeRange(const uint8_t *frame, size_t length, uint8_t sequence,
                     uint32_t poll_tx, uint32_t response_rx,
                     int16_t clock_offset, int32_t *distance_mm)
{
    static const uint8_t header[] = {0x9a, 0x60, 'V', 'E', 'W', 'A', 0xe1};
    if (frame == NULL || distance_mm == NULL || length != 20U ||
        frame[0] != 0x41 || frame[1] != 0x88 || frame[2] != sequence ||
        memcmp(frame + 3, header, sizeof header) != 0) return false;

    /* Modulo subtraction handles low-32-bit rollover (~67 ms). */
    uint32_t round_trip = response_rx - poll_tx;
    uint32_t turnaround = get_le32(frame + 14) - get_le32(frame + 10);
    double offset = (double)clock_offset / 67108864.0;
    double tof = ((double)round_trip - (double)turnaround * (1.0 - offset)) / 2.0;
    double metres = tof * (1.0 / 499.2e6 / 128.0) * 299702547.0;
    if (!isfinite(metres) || metres < 0.0 || metres > 1000.0 ||
        turnaround > 638976000U || round_trip > 1277952000U) return false;
    *distance_mm = (int32_t)(metres * 1000.0 + 0.5);
    return true;
}
