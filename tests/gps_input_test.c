#include "gps_input.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    puts("GPS_INPUT tests passed: conversion, timing, validity, ignore flags and MAVLink wire roundtrip.");
    return 0;
}
