#include "gps_input.h"
#include "gps_ubx_fixture.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// MinGW defaults to Microsoft structure layout; use -mno-ms-bitfields there.
_Static_assert(sizeof(mavlink_gps_input_t) == 65, "MAVLink packing must match ARM/wire layout");
_Static_assert(offsetof(mavlink_gps_input_t, yaw) == 63, "GPS_INPUT extension offset");

static GPS_RawSnapshot_t Sample(void)
{
    GPS_RawSnapshot_t gps = {0};
    gps.rmc_decoded_count = gps.gga_count = gps.gsa_decoded_count = 1;
    gps.last_rmc_ms = gps.last_gga_ms = gps.last_gsa_ms = 1000;
    gps.fix.position_valid = gps.fix.altitude_valid = gps.fix.hdop_valid = true;
    gps.fix.time_valid = true;
    gps.fix.utc_ms = 32835000;
    gps.fix.latitude_e7 = 223364238;
    gps.fix.longitude_e7 = 1142680587;
    gps.fix.altitude_msl_mm = 40200;
    gps.fix.hdop_x100 = 141;
    gps.fix.satellites_used = 11;
    gps.fix.fix_quality = 1;
    gps.gsa.fix_dimension = 3;
    gps.gsa.dop_valid = true;
    gps.gsa.vdop_x100 = 198;
    gps.rmc.position_valid = gps.rmc.time_valid = true;
    gps.rmc.year = 2026;
    gps.rmc.month = 10;
    gps.rmc.day = 27;
    gps.rmc.utc_ms = gps.fix.utc_ms;
    gps.rmc.speed_valid = gps.rmc.course_valid = true;
    gps.rmc.speed_mm_s = 1000;
    gps.rmc.course_cdeg = 9000; // East at 1 m/s.
    return gps;
}

int main(void)
{
    GPS_RawSnapshot_t gps = Sample();
    mavlink_gps_input_t input;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.fix_type == GPS_FIX_TYPE_3D_FIX && input.gps_id == 0);
    assert(input.lat == 223364238 && input.lon == 1142680587);
    assert(fabsf(input.alt - 40.2f) < 0.001f);
    assert(fabsf(input.hdop - 1.41f) < 0.001f && fabsf(input.vdop - 1.98f) < 0.001f);
    assert(input.satellites_visible == 11 && input.yaw == 0);
    assert(fabsf(input.vn) < 0.0001f && fabsf(input.ve - 1.0f) < 0.0001f);
    // Independently calculated using Python datetime, including 18s GPS-UTC offset.
    assert(input.time_usec == 1793092035000000ULL);
    assert(input.time_week == 2442 && input.time_week_ms == 205653000);
    assert(input.ignore_flags == (GPS_INPUT_IGNORE_FLAG_VEL_VERT |
        GPS_INPUT_IGNORE_FLAG_SPEED_ACCURACY | GPS_INPUT_IGNORE_FLAG_HORIZONTAL_ACCURACY |
        GPS_INPUT_IGNORE_FLAG_VERTICAL_ACCURACY));

    // End-to-end MAVLink v2 framing/CRC/decoding.
    mavlink_message_t message, decoded_message;
    mavlink_status_t status = {0};
    mavlink_msg_gps_input_encode_chan(1, MAV_COMP_ID_ONBOARD_COMPUTER,
                                    MAVLINK_COMM_0, &message, &input);
    uint8_t bytes[MAVLINK_MAX_PACKET_LEN];
    uint16_t length = mavlink_msg_to_send_buffer(bytes, &message);
    assert(bytes[0] == 0xFD);
    unsigned int messages = 0;
    for (uint16_t i = 0; i < length; ++i)
        messages += mavlink_parse_char(MAVLINK_COMM_1, bytes[i], &decoded_message, &status);
    assert(messages == 1 && decoded_message.msgid == MAVLINK_MSG_ID_GPS_INPUT);
    mavlink_gps_input_t decoded;
    mavlink_msg_gps_input_decode(&decoded_message, &decoded);
    assert(decoded.lat == input.lat && decoded.time_week_ms == input.time_week_ms);
    assert(decoded.ignore_flags == input.ignore_flags && decoded.fix_type == input.fix_type);

    gps.rmc.course_valid = false;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.ignore_flags & GPS_INPUT_IGNORE_FLAG_VEL_HORIZ);
    assert(input.vn == 0 && input.ve == 0);
    gps.fix.altitude_valid = false;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.fix_type == GPS_FIX_TYPE_2D_FIX);
    assert(input.ignore_flags & GPS_INPUT_IGNORE_FLAG_ALT);
    gps.fix.hdop_valid = gps.gsa.dop_valid = false;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.ignore_flags & GPS_INPUT_IGNORE_FLAG_HDOP);
    assert(input.ignore_flags & GPS_INPUT_IGNORE_FLAG_VDOP);

    gps = Sample();
    gps.fix.fix_quality = 4;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_RTK_FIXED);
    gps.fix.fix_quality = 5;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_RTK_FLOAT);
    gps.fix.fix_quality = 2;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_DGPS);
    gps.gsa.fix_dimension = 1;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_NO_FIX);
    assert(input.lat == 0 && input.lon == 0);
    gps = Sample();
    gps.rmc.position_valid = false;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_NO_FIX);
    assert(input.ignore_flags == 255U);
    assert(!GPS_BuildInput(&gps, 2500, &input)); // Stale data never retransmitted.

    gps = Sample();
    gps.fix.utc_ms++;
    assert(!GPS_BuildInput(&gps, 1100, &input)); // Different epochs.
    gps = Sample();
    gps.last_gsa_ms = 400;
    assert(!GPS_BuildInput(&gps, 1100, &input)); // GSA from an older cycle.
    gps = Sample();
    gps.last_gsa_ms = 1501;
    assert(!GPS_BuildInput(&gps, 1510, &input));
    gps = Sample();
    gps.rmc.time_valid = false;
    assert(!GPS_BuildInput(&gps, 1100, &input));
    gps = Sample();
    gps.rmc.year = 2016;
    assert(!GPS_BuildInput(&gps, 1100, &input));
    gps = Sample();
    gps.gga_count = 0;
    assert(!GPS_BuildInput(&gps, 1100, &input));
    gps = Sample();
    gps.last_rmc_ms = gps.last_gga_ms = gps.last_gsa_ms = UINT32_MAX - 50U;
    assert(GPS_BuildInput(&gps, 50, &input)); // Tick counter rollover.
    assert(!GPS_BuildInput(NULL, 0, &input));
    assert(!GPS_BuildInput(&gps, 0, NULL));

    // UBX preference is independent of NMEA availability and supplies measured
    // velocity/accuracy without requiring a course-over-ground field.
    memset(&gps, 0, sizeof(gps));
    assert(GPS_UbxDecodePvt(ubx_pvt_fixture + 6, 92, &gps.pvt));
    gps.pvt_count = 1;
    gps.last_pvt_ms = 1000;
    assert(!GPS_BuildInput(&gps, 1100, &input)); // Wait for this epoch's NAV-DOP.
    gps.dop_count = 1;
    gps.last_dop_ms = 1000;
    gps.dop.tow_ms = gps.pvt.tow_ms - 100;
    gps.dop.hdop_x100 = 141;
    gps.dop.vdop_x100 = 198;
    assert(!GPS_BuildInput(&gps, 1100, &input)); // Previous epoch is not reused.
    gps.dop.tow_ms = gps.pvt.tow_ms;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.fix_type == GPS_FIX_TYPE_3D_FIX && input.yaw == 0);
    assert(input.lat == 223364238 && input.lon == 1142680587);
    assert(fabsf(input.alt - 40.2f) < .001f);
    assert(fabsf(input.vn - 1.25f) < .0001f && fabsf(input.ve + 2.5f) < .0001f);
    assert(fabsf(input.vd - .375f) < .0001f);
    assert(fabsf(input.horiz_accuracy - .8f) < .0001f);
    assert(fabsf(input.vert_accuracy - 1.4f) < .0001f);
    assert(fabsf(input.speed_accuracy - .05f) < .0001f);
    assert(input.ignore_flags == 0);
    assert(fabsf(input.hdop - 1.41f) < .001f && fabsf(input.vdop - 1.98f) < .001f);
    assert(input.time_usec == 1793092035123456ULL);
    assert(input.time_week == 2442 && input.time_week_ms == 205653123);
    mavlink_msg_gps_input_encode_chan(1, MAV_COMP_ID_ONBOARD_COMPUTER,
                                    MAVLINK_COMM_0, &message, &input);
    length = mavlink_msg_to_send_buffer(bytes, &message);
    messages = 0;
    for (uint16_t i = 0; i < length; ++i)
        messages += mavlink_parse_char(MAVLINK_COMM_1, bytes[i], &decoded_message, &status);
    assert(messages == 1);
    mavlink_msg_gps_input_decode(&decoded_message, &decoded);
    assert(decoded.ignore_flags == 0 && decoded.yaw == 0);
    assert(fabsf(decoded.ve + 2.5f) < .0001f && fabsf(decoded.vd - .375f) < .0001f);
    assert(fabsf(decoded.speed_accuracy - .05f) < .0001f);
    assert(!GPS_BuildInput(&gps, 1500, &input));
    gps.rmc_decoded_count = 2;
    gps.last_rmc_ms = 1499;
    assert(!GPS_BuildInput(&gps, 1500, &input)); // No silent NMEA fallback after UBX stops.
    gps.pvt.velocity_n_mm_s = gps.pvt.velocity_e_mm_s = gps.pvt.velocity_d_mm_s = 0;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.vn == 0 && input.ve == 0 && input.vd == 0);
    assert(!(input.ignore_flags & GPS_INPUT_IGNORE_FLAG_VEL_HORIZ));
    gps.pvt.fix_type = 2;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.fix_type == GPS_FIX_TYPE_2D_FIX);
    assert(input.ignore_flags & GPS_INPUT_IGNORE_FLAG_VEL_VERT);
    assert(input.ignore_flags & GPS_INPUT_IGNORE_FLAG_ALT);
    gps.pvt.fix_ok = false;
    gps.dop_count = 0; // Loss of fix is forwarded even without NAV-DOP.
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.fix_type == GPS_FIX_TYPE_NO_FIX && input.ignore_flags == 255);
    assert(input.lat == 0 && input.vn == 0 && input.ve == 0);
    gps.dop_count = 1;
    gps.pvt.fix_ok = true;
    gps.pvt.fix_type = 3;
    gps.pvt.flags = 0x81;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_RTK_FIXED);
    gps.pvt.flags = 0x41;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_RTK_FLOAT);
    gps.pvt.flags = 3;
    assert(GPS_BuildInput(&gps, 1100, &input) && input.fix_type == GPS_FIX_TYPE_DGPS);
    gps.pvt.nano = -100000000;
    gps.pvt.tow_ms = 205652900;
    gps.dop.tow_ms = gps.pvt.tow_ms;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.time_usec == 1793092034900000ULL && input.time_week_ms == 205652900);
    gps.pvt.valid = 0;
    assert(GPS_BuildInput(&gps, 1100, &input));
    assert(input.time_usec == 1100000 && input.time_week == 0);
    gps.last_dop_ms = gps.last_pvt_ms = UINT32_MAX - 20U;
    assert(GPS_BuildInput(&gps, 50, &input));
    puts("GPS_INPUT tests passed: conversion, timing, validity, ignore flags and MAVLink wire roundtrip.");
    return 0;
}
