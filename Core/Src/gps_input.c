#include "gps_input.h"
#include <math.h>
#include <string.h>

#define GPS_FRESH_MS 1500U
#define GPS_SENTENCE_SKEW_MS 500U

static bool GpsTime(const GPS_NmeaRmc_t *rmc, mavlink_gps_input_t *out)
{
    // GPS-UTC is 18 seconds from 2017-01-01. Update if a new leap second is announced.
    if (!rmc->time_valid || rmc->year < 2017U || rmc->year > 2099U ||
        rmc->month < 1U || rmc->month > 12U || rmc->day < 1U || rmc->day > 31U ||
        rmc->utc_ms >= 86400000U) return false;
    uint32_t days = 0;
    for (uint32_t year = 1970U; year < rmc->year; ++year)
        days += 365U + (year % 4U == 0U);
    static const uint8_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (uint32_t month = 1; month < rmc->month; ++month)
        days += month_days[month - 1U] + (month == 2U && rmc->year % 4U == 0U);
    days += rmc->day - 1U;
    uint64_t unix_ms = (uint64_t)days * 86400000ULL + rmc->utc_ms;
    uint64_t gps_ms = unix_ms - 315964800000ULL + 18000ULL;
    out->time_usec = unix_ms * 1000ULL;
    out->time_week = (uint16_t)(gps_ms / 604800000ULL);
    out->time_week_ms = (uint32_t)(gps_ms % 604800000ULL);
    return true;
}

bool GPS_BuildInput(const GPS_RawSnapshot_t *gps, uint32_t now_ms,
                    mavlink_gps_input_t *out)
{
    if (gps == NULL || out == NULL || gps->rmc_decoded_count == 0U ||
        (uint32_t)(now_ms - gps->last_rmc_ms) >= GPS_FRESH_MS) return false;

    mavlink_gps_input_t next = {0};
    next.gps_id = 0; // ArduPilot GPS1, independent of SERIAL3's port number.
    next.fix_type = GPS_FIX_TYPE_NO_FIX;
    next.hdop = UINT16_MAX;
    next.vdop = UINT16_MAX;
    next.ignore_flags = GPS_INPUT_IGNORE_FLAG_ALT | GPS_INPUT_IGNORE_FLAG_HDOP |
        GPS_INPUT_IGNORE_FLAG_VDOP | GPS_INPUT_IGNORE_FLAG_VEL_HORIZ |
        GPS_INPUT_IGNORE_FLAG_VEL_VERT | GPS_INPUT_IGNORE_FLAG_SPEED_ACCURACY |
        GPS_INPUT_IGNORE_FLAG_HORIZONTAL_ACCURACY | GPS_INPUT_IGNORE_FLAG_VERTICAL_ACCURACY;
    GpsTime(&gps->rmc, &next);
    if (!gps->rmc.position_valid)
    {
        *out = next;
        return true;
    }
    // GGA and RMC must describe exactly the same UTC measurement epoch.
    // GSA has no timestamp, so require it to arrive within 500ms of RMC.
    if (!gps->rmc.time_valid || gps->gga_count == 0U || gps->gsa_decoded_count == 0U ||
        (uint32_t)(now_ms - gps->last_gga_ms) >= GPS_FRESH_MS ||
        (uint32_t)(now_ms - gps->last_gsa_ms) >= GPS_FRESH_MS ||
        !gps->fix.time_valid || gps->fix.utc_ms != gps->rmc.utc_ms)
        return false;
    uint32_t skew = gps->last_gsa_ms - gps->last_rmc_ms;
    if (skew > GPS_SENTENCE_SKEW_MS && (0U - skew) > GPS_SENTENCE_SKEW_MS)
        return false;
    if (!gps->fix.position_valid || gps->gsa.fix_dimension < 2U)
    {
        *out = next;
        return true;
    }
    if (!GpsTime(&gps->rmc, &next)) return false;
    next.fix_type = GPS_FIX_TYPE_2D_FIX;
    if (gps->gsa.fix_dimension == 3U && gps->fix.altitude_valid)
    {
        next.fix_type = GPS_FIX_TYPE_3D_FIX;
        if (gps->fix.fix_quality == 2U) next.fix_type = GPS_FIX_TYPE_DGPS;
        if (gps->fix.fix_quality == 4U) next.fix_type = GPS_FIX_TYPE_RTK_FIXED;
        if (gps->fix.fix_quality == 5U) next.fix_type = GPS_FIX_TYPE_RTK_FLOAT;
        next.alt = gps->fix.altitude_msl_mm * 0.001f;
        next.ignore_flags &= ~GPS_INPUT_IGNORE_FLAG_ALT;
    }
    next.lat = gps->fix.latitude_e7;
    next.lon = gps->fix.longitude_e7;
    next.satellites_visible = gps->fix.satellites_used;
    if (gps->fix.hdop_valid && gps->fix.hdop_x100 < 65535U)
    {
        next.hdop = gps->fix.hdop_x100 * 0.01f;
        next.ignore_flags &= ~GPS_INPUT_IGNORE_FLAG_HDOP;
    }
    if (gps->gsa.dop_valid && gps->gsa.vdop_x100 < 65535U)
    {
        next.vdop = gps->gsa.vdop_x100 * 0.01f;
        next.ignore_flags &= ~GPS_INPUT_IGNORE_FLAG_VDOP;
    }
    if (gps->rmc.speed_valid && gps->rmc.course_valid)
    {
        float radians = gps->rmc.course_cdeg * (0.0174532925199433f / 100.0f);
        float speed = gps->rmc.speed_mm_s * 0.001f;
        next.vn = speed * cosf(radians);
        next.ve = speed * sinf(radians);
        next.ignore_flags &= ~GPS_INPUT_IGNORE_FLAG_VEL_HORIZ;
    }
    // Vertical velocity and metric accuracy are not available in these NMEA messages.
    // Course of travel is deliberately not sent as vehicle yaw.
    *out = next;
    return true;
}
