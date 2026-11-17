#include "gps_ubx.h"
#include "gps_diag.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    const uint16_t mask = (1U << 5) | (1U << 13);
    assert(GPS_UbxM9StartupProfile(mask, mask, false) == 0);
    assert(GPS_UbxM9StartupProfile(0, 0, false) == 0);
    assert(GPS_UbxM9StartupProfile(1U << 5, 0, false) == 0);
    assert(GPS_UbxM9StartupProfile(mask, 1U << 5, false) == 3);
    assert(GPS_UbxM9StartupProfile(mask, 1U << 13, false) == 3);
    assert(GPS_UbxM9StartupProfile(mask, 0, true) == 0);

    uint8_t packet[64], baseline[64];
    size_t base = GPS_UbxBuildRateConfig(baseline, sizeof(baseline), 0, 200);
    size_t size = GPS_UbxBuildRateConfig(packet, sizeof(packet), 3, 200);
    const uint8_t keys[] = {0x25,0,0x31,0x10,1,0x18,0,0x31,0x10,1};
    assert(base == 44 && size == 54 && packet[7] == 1);
    assert(memcmp(packet + 6, baseline + 6, base - 8) == 0);
    assert(memcmp(packet + base - 2, keys, sizeof(keys)) == 0);
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < size - 2; ++i) { a += packet[i]; b += a; }
    assert(packet[size-2] == a && packet[size-1] == b);

    /* A real CFG-VALGET-shaped ANA response must decode as known/enabled. */
    uint8_t reply[] = {0xb5,0x62,6,0x8b,9,0,1,0,0,0,1,0,0x23,0x10,1,0,0};
    a = b = 0;
    for (size_t i = 2; i < sizeof(reply)-2; ++i) { a += reply[i]; b += a; }
    reply[15] = a; reply[16] = b;
    static GPS_DiagParser_t parser;
    GPS_DiagSnapshot_t snapshot = {0};
    for (size_t i = 0; i < sizeof(reply); ++i)
        GPS_DiagFeed(&parser, reply[i], &snapshot, 100);
    assert(snapshot.filter_known == (1UL << 22));
    assert(snapshot.filters[22] == 1 && snapshot.bad_frames == 0);
    return 0;
}
