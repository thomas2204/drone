#include "gps_ubx.h"
#include "gps_ubx_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static GPS_UbxEvent_t Feed(GPS_UbxParser_t *p, const uint8_t *bytes, size_t size)
{
    GPS_UbxEvent_t event = GPS_UBX_NONE;
    for (size_t i = 0; i < size; ++i)
    {
        GPS_UbxEvent_t next = GPS_UbxFeed(p, bytes[i]);
        if (next != GPS_UBX_NONE) event = next;
    }
    return event;
}

static void Checksum(uint8_t *frame, size_t size)
{
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < size - 2U; ++i)
    {
        a += frame[i];
        b += a;
    }
    frame[size - 2U] = a;
    frame[size - 1U] = b;
}

int main(void)
{
    GPS_UbxParser_t p;
    GPS_UbxReset(&p);
    // Split arbitrarily as DMA/ring chunks would; incomplete data is not a fix.
    assert(Feed(&p, ubx_pvt_fixture, 37) == GPS_UBX_NONE);
    assert(GPS_UbxBusy(&p));
    assert(Feed(&p, ubx_pvt_fixture + 37, sizeof(ubx_pvt_fixture) - 37) == GPS_UBX_PVT);
    assert(!GPS_UbxBusy(&p));
    assert(p.pvt.tow_ms == 205653123U && p.pvt.fix_ok && p.pvt.fix_type == 3);
    assert(p.pvt.latitude_e7 == 223364238 && p.pvt.longitude_e7 == 1142680587);
    assert(p.pvt.altitude_msl_mm == 40200); // Not ellipsoid height.
    assert(p.pvt.velocity_n_mm_s == 1250 && p.pvt.velocity_e_mm_s == -2500);
    assert(p.pvt.velocity_d_mm_s == 375 && p.pvt.ground_speed_mm_s == 2795);
    assert(p.pvt.horizontal_accuracy_mm == 800 && p.pvt.vertical_accuracy_mm == 1400);
    assert(p.pvt.speed_accuracy_mm_s == 50 && p.pvt.nano == 123456789);
    assert(p.pvt.year == 2026 && p.pvt.month == 10 && p.pvt.day == 27);
    assert(p.pvt.hour == 9 && p.pvt.minute == 7 && p.pvt.second == 15);
    assert(p.pvt.valid == 7 && p.pvt.satellites == 11);

    uint8_t broken[sizeof(ubx_pvt_fixture)];
    memcpy(broken, ubx_pvt_fixture, sizeof(broken));
    broken[50] ^= 1U;
    assert(Feed(&p, broken, sizeof(broken)) == GPS_UBX_BAD_FRAME);
    assert(Feed(&p, ubx_pvt_fixture, sizeof(ubx_pvt_fixture)) == GPS_UBX_PVT);
    // A bad checksum ending in sync byte must allow resynchronizing at 0x62.
    broken[sizeof(broken) - 1U] = 0xB5;
    assert(Feed(&p, broken, sizeof(broken)) == GPS_UBX_BAD_FRAME);
    assert(Feed(&p, ubx_pvt_fixture + 1, sizeof(ubx_pvt_fixture) - 1) == GPS_UBX_PVT);
    GPS_UbxReset(&p);
    assert(Feed(&p, ubx_pvt_fixture, sizeof(ubx_pvt_fixture) - 2) == GPS_UBX_NONE);
    assert(Feed(&p, ubx_pvt_fixture, 2) == GPS_UBX_BAD_FRAME);
    assert(Feed(&p, ubx_pvt_fixture + 2, sizeof(ubx_pvt_fixture) - 2) == GPS_UBX_PVT);

    static const uint8_t dop[] = {
        0xB5,0x62,0x01,0x04,0x12,0x00,0x83,0x04,0x42,0x0C,
        0xC8,0x00,0x8D,0x00,0x50,0x00,0xC6,0x00,0x8D,0x00,
        0x64,0x00,0x78,0x00,0xC0,0xCA
    };
    assert(Feed(&p, dop, sizeof(dop)) == GPS_UBX_DOP);
    assert(p.dop.tow_ms == 205653123 && p.dop.hdop_x100 == 141 && p.dop.vdop_x100 == 198);

    uint8_t ack[] = {0xB5,0x62,0x05,0x01,0x02,0x00,0x06,0x8A,0x98,0xC1};
    uint8_t nak[] = {0xB5,0x62,0x05,0x00,0x02,0x00,0x06,0x8A,0x97,0xBC};
    assert(Feed(&p, ack, sizeof(ack)) == GPS_UBX_ACK);
    assert(p.acknowledged_class == 6 && p.acknowledged_id == 0x8A);
    assert(Feed(&p, nak, sizeof(nak)) == GPS_UBX_NAK);

    // A valid unknown message larger than storage is checksum-consumed safely.
    uint8_t large[208] = {0xB5,0x62,0x01,0x35,0xC8,0x00};
    memset(large + 6, 0x24, 200);
    Checksum(large, sizeof(large));
    assert(Feed(&p, large, sizeof(large)) == GPS_UBX_NONE);
    assert(Feed(&p, ubx_pvt_fixture, sizeof(ubx_pvt_fixture)) == GPS_UBX_PVT);
    uint8_t oversized[] = {0xB5,0x62,0x01,0x07,0x01,0x04};
    assert(Feed(&p, oversized, sizeof(oversized)) == GPS_UBX_BAD_FRAME);
    assert(Feed(&p, ubx_pvt_fixture, sizeof(ubx_pvt_fixture)) == GPS_UBX_PVT);

    GPS_UbxPvt_t sample;
    uint8_t payload[92];
    memcpy(payload, ubx_pvt_fixture + 6, sizeof(payload));
    payload[21] = 0;
    assert(GPS_UbxDecodePvt(payload, sizeof(payload), &sample) && !sample.fix_ok);
    payload[21] = 1;
    payload[78] = 1;
    assert(GPS_UbxDecodePvt(payload, sizeof(payload), &sample) && !sample.fix_ok);
    payload[78] = 0;
    payload[28] = payload[29] = payload[30] = payload[31] = 0x7F;
    GPS_UbxPvt_t unchanged = sample;
    assert(!GPS_UbxDecodePvt(payload, sizeof(payload), &sample));
    assert(memcmp(&sample, &unchanged, sizeof(sample)) == 0);
    assert(!GPS_UbxDecodePvt(payload, 91, &sample));
    assert(!GPS_UbxDecodePvt(NULL, 92, &sample));

    static const uint8_t config_expected[] = {
        0xB5,0x62,0x06,0x8A,0x24,0x00,0x00,0x01,0x00,0x00,
        0x01,0x00,0x21,0x30,0x64,0x00,0x02,0x00,0x21,0x30,0x01,0x00,
        0x07,0x00,0x91,0x20,0x01,0x39,0x00,0x91,0x20,0x01,
        0x01,0x00,0x74,0x10,0x01,0x02,0x00,0x74,0x10,0x00,0x6F,0x54
    };
    uint8_t config[48];
    assert(GPS_UbxBuild10HzConfig(config, sizeof(config)) == sizeof(config_expected));
    assert(memcmp(config, config_expected, sizeof(config_expected)) == 0);
    assert(config[7] == 1); // RAM only, no BBR/flash writes.
    assert(GPS_UbxBuild10HzConfig(config, 43) == 0);
    assert(GPS_UbxBuild10HzConfig(NULL, sizeof(config)) == 0);

    sample = p.pvt;
    sample.latitude_e7 = -1;
    sample.longitude_e7 = -123456789;
    sample.altitude_msl_mm = -120;
    char text[192];
    GPS_UbxFormatPvt(text, sizeof(text), &sample);
    assert(strstr(text, "Lat=-0.0000001") && strstr(text, "Lon=-12.3456789"));
    assert(strstr(text, "AltMSL=-0.120"));
    sample.fix_ok = false;
    GPS_UbxFormatPvt(text, sizeof(text), &sample);
    assert(strstr(text, "NO FIX"));
    puts("UBX tests passed: independent frame/config fixtures, signed fields, checksums, recovery, ACK/NAK and validity.");
    return 0;
}
