#include "uwb_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
    for (int i = 0; i < 4; ++i) { p[i] = (uint8_t)value; value >>= 8; }
}

int main(void)
{
    uint8_t frame[20] = {0x41,0x88,255,0x9a,0x60,'V','E','W','A',0xe1};
    int32_t mm;
    uint32_t turnaround = 1500U * 63898U;
    /* Both initiator and beacon counters wrap during this exchange.
     * 4264 round-trip propagation ticks correspond to approximately 10 m. */
    uint32_t beacon_rx = 0xff000000U, poll_tx = 0xff800000U;
    put32(frame + 10, beacon_rx);
    put32(frame + 14, beacon_rx + turnaround);
    assert(UWB_DecodeRange(frame, 20, 255, poll_tx,
        poll_tx + turnaround + 4264U, 0, &mm));
    assert(mm >= 9990 && mm <= 10010);
    /* New 850 kbps responder uses a 2.5 ms turnaround. It is carried in
     * the response, so the initiator must not assume the old 1.5 ms value. */
    uint32_t new_turnaround = 2500U * 63898U;
    put32(frame + 14, beacon_rx + new_turnaround);
    assert(UWB_DecodeRange(frame, 20, 255, poll_tx,
        poll_tx + new_turnaround + 4264U, 0, &mm));
    assert(mm >= 9990 && mm <= 10010);
    put32(frame + 14, beacon_rx + turnaround);
    /* Verify correction sign for each clock drift direction. */
    for (int offset = -1200; offset <= 1200; offset += 2400) {
        uint32_t local_delay = (uint32_t)(turnaround * (1.0 - offset / 67108864.0) + 0.5);
        assert(UWB_DecodeRange(frame, 20, 255, poll_tx,
            poll_tx + local_delay + 4264U, (int16_t)offset, &mm));
        assert(mm >= 9990 && mm <= 10010);
    }
    assert(!UWB_DecodeRange(frame, 19, 255, poll_tx, poll_tx + turnaround + 4264, 0, &mm));
    assert(!UWB_DecodeRange(frame, 20, 0, poll_tx, poll_tx + turnaround + 4264, 0, &mm));
    frame[9] = 0xe0;
    assert(!UWB_DecodeRange(frame, 20, 255, poll_tx, poll_tx + turnaround + 4264, 0, &mm));
    frame[9] = 0xe1;
    assert(!UWB_DecodeRange(frame, 20, 255, poll_tx, poll_tx + turnaround - 100, 0, &mm));
    assert(!UWB_DecodeRange(frame, 20, 255, poll_tx, poll_tx + turnaround + 1000000, 0, &mm));
    puts("UWB protocol tests passed (distance, rollover, drift, invalid frames).");
}
