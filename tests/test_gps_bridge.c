#include "gps_input.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void near(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.00001f);
}

/* Independent UBX wire fixture: deliberately distinct adjacent fields catch
 * offset mistakes. Exercise values on both sides of EKF's 1 m/s check. */
static void check_accuracy(uint32_t sacc)
{
    uint8_t payload[92] = {0};
    put32(payload, 310366000);
    payload[4] = 0xea; payload[5] = 7; /* 2026 */
    payload[6] = 11; payload[7] = 18;
    payload[8] = 14; payload[9] = 11; payload[10] = 46;
    payload[11] = 7;
    payload[20] = 3; payload[21] = 1; payload[23] = 15;
    put32(payload + 24, 1142686269);
    put32(payload + 28, 223363418);
    put32(payload + 32, 40234); /* ellipsoid height must not replace MSL */
    put32(payload + 36, 29990);
    put32(payload + 40, 3110);
    put32(payload + 44, 4220);
    put32(payload + 48, (uint32_t)-287);
    put32(payload + 52, 6);
    put32(payload + 56, (uint32_t)-111);
    put32(payload + 60, 287);
    put32(payload + 64, 18119765); /* heading must not replace sAcc or yaw */
    put32(payload + 68, sacc);
    put32(payload + 72, 123456);
    GPS_UbxParser_t parser = {0};
    uint8_t packet[100] = {0xb5, 0x62, 1, 7, 92, 0};
    memcpy(packet + 6, payload, sizeof(payload));
    uint8_t a = 0, b = 0;
    for (unsigned i = 2; i < 98; ++i) { a += packet[i]; b += a; }
    packet[98] = a; packet[99] = b;
    GPS_UbxEvent_t event = GPS_UBX_NONE;
    for (unsigned i = 0; i < sizeof(packet); ++i)
        event = GPS_UbxFeed(&parser, packet[i]);
    assert(event == GPS_UBX_PVT && parser.pvt.fix_ok);
    GPS_RawSnapshot_t gps = {0};
    gps.pvt = parser.pvt;
    gps.pvt_count = gps.dop_count = 1;
    gps.last_pvt_ms = gps.last_dop_ms = 10000;
    gps.dop.tow_ms = gps.pvt.tow_ms;
    gps.dop.hdop_x100 = 88; gps.dop.vdop_x100 = 114;
    mavlink_gps_input_t input, decoded;
    assert(GPS_BuildInput(&gps, 10005, &input));
    mavlink_message_t message;
    mavlink_msg_gps_input_encode(1, MAV_COMP_ID_ONBOARD_COMPUTER, &message, &input);
    mavlink_msg_gps_input_decode(&message, &decoded);
    assert(decoded.fix_type == 3 && decoded.satellites_visible == 15);
    assert(decoded.lat == 223363418 && decoded.lon == 1142686269);
    assert(decoded.ignore_flags == 0 && decoded.yaw == 0);
    assert(decoded.time_week_ms == 310366000);
    near(decoded.alt, 29.990f);
    near(decoded.vn, -0.287f); near(decoded.ve, 0.006f); near(decoded.vd, -0.111f);
    near(decoded.horiz_accuracy, 3.110f); near(decoded.vert_accuracy, 4.220f);
    near(decoded.speed_accuracy, sacc * 0.001f);
    near(decoded.hdop, 0.88f); near(decoded.vdop, 1.14f);
    /* Incomplete/stale measurements must not silently become fresh fixes. */
    gps.dop.tow_ms -= 200;
    assert(!GPS_BuildInput(&gps, 10005, &input));
    gps.dop.tow_ms = gps.pvt.tow_ms;
    assert(!GPS_BuildInput(&gps, 10500, &input));
    gps.pvt.fix_ok = false;
    assert(GPS_BuildInput(&gps, 10005, &input) && input.fix_type == 1);
    packet[68] ^= 1;
    for (unsigned i = 0; i < sizeof(packet); ++i)
        event = GPS_UbxFeed(&parser, packet[i]);
    assert(event == GPS_UBX_BAD_FRAME);
}

int main(void)
{
    _Static_assert(sizeof(mavlink_gps_input_t) == 65, "Match ARM MAVLink packing");
    /* Independent whitelist for the normal 5-Hz startup command. No reset,
     * constellation, assistance or persistent-storage keys may be emitted. */
    uint8_t command[64];
    const uint8_t expected[] = {
        0xb5,0x62,6,0x8a,36,0, 0,1,0,0,
        1,0,0x21,0x30,200,0, 2,0,0x21,0x30,1,0,
        7,0,0x91,0x20,1, 0x39,0,0x91,0x20,1,
        1,0,0x74,0x10,1, 2,0,0x74,0x10,0
    };
    assert(GPS_UbxBuildRateConfig(command, sizeof(command), 0, 200) == 44);
    assert(memcmp(command, expected, sizeof(expected)) == 0);
    check_accuracy(0);
    check_accuracy(610);
    check_accuracy(1000);
    check_accuracy(1010);
    check_accuracy(1070);
    check_accuracy(1560);
    puts("GPS UBX-to-MAVLink accuracy, velocity, epoch, freshness and checksum checks passed");
    return 0;
}
