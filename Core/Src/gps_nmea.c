#include "gps_nmea.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Fixed-point decimal parsing: no heap allocation or floating-point printf.
// scale is a power of ten. Extra fractional digits are truncated.
static bool ParseScaled(const char *text, uint32_t scale, int64_t *out)
{
    bool negative = false;
    if (*text == '-')
    {
        negative = true;
        ++text;
    }
    if (*text < '0' || *text > '9') return false;

    int64_t whole = 0;
    while (*text >= '0' && *text <= '9')
    {
        whole = whole * 10 + (*text++ - '0');
        if (whole > INT32_MAX) return false;
    }

    int64_t fraction = 0;
    uint32_t place = scale / 10U;
    if (*text == '.')
    {
        ++text;
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9')
        {
            fraction += (int64_t)(*text++ - '0') * place;
            place /= 10U;
        }
    }
    if (*text != '\0') return false;

    int64_t value = whole * scale + fraction;
    *out = negative ? -value : value;
    return true;
}

static bool ParseUnsigned(const char *text, uint32_t max, uint32_t *out)
{
    if (*text < '0' || *text > '9') return false;
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9')
    {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > max / 10U ||
            (value == max / 10U && digit > max % 10U)) return false;
        value = value * 10U + digit;
    }
    if (*text != '\0') return false;
    *out = value;
    return true;
}

static bool ParseCoordinate(const char *text, const char *hemisphere,
                            bool latitude, int32_t *out)
{
    size_t digits = strcspn(text, ".");
    if (digits != (latitude ? 4U : 5U) ||
        *text < '0' || *text > '9' || strlen(hemisphere) != 1U)
        return false;

    char positive = latitude ? 'N' : 'E';
    char negative = latitude ? 'S' : 'W';
    if (*hemisphere != positive && *hemisphere != negative) return false;

    int64_t raw;
    if (!ParseScaled(text, 10000000U, &raw)) return false;
    int64_t degrees = raw / 1000000000LL;
    int64_t minutes_e7 = raw % 1000000000LL;
    int64_t max_degrees = latitude ? 90 : 180;
    if (degrees > max_degrees || minutes_e7 >= 600000000LL ||
        (degrees == max_degrees && minutes_e7 != 0)) return false;

    // NMEA ddmm.mmmm -> decimal degrees, rounded to 10^-7 degree.
    int64_t value = degrees * 10000000LL + (minutes_e7 + 30) / 60;
    *out = (int32_t)(*hemisphere == negative ? -value : value);
    return true;
}

GPS_NmeaResult_t GPS_NmeaParseGga(const char *line, GPS_NmeaFix_t *out)
{
    if (line == NULL || out == NULL || strlen(line) < 7U ||
        line[0] != '$' || memcmp(line + 3, "GGA,", 4) != 0)
        return GPS_NMEA_IGNORED;

    const char *star = strchr(line, '*');
    if (star == NULL || strlen(star) != 3U)
        return GPS_NMEA_BAD_CHECKSUM;
    int high = HexDigit(star[1]);
    int low = HexDigit(star[2]);
    if (high < 0 || low < 0) return GPS_NMEA_BAD_CHECKSUM;

    uint8_t checksum = 0;
    for (const char *p = line + 1; p < star; ++p) checksum ^= (uint8_t)*p;
    if (checksum != (uint8_t)((high << 4) | low))
        return GPS_NMEA_BAD_CHECKSUM;

    char body[128];
    size_t body_size = (size_t)(star - line - 1);
    if (body_size >= sizeof(body)) return GPS_NMEA_BAD_GGA;
    memcpy(body, line + 1, body_size);
    body[body_size] = '\0';

    // Preserve empty fields; strtok() would incorrectly skip them.
    char *fields[20];
    size_t count = 1;
    fields[0] = body;
    for (char *p = body; *p != '\0'; ++p)
    {
        if (*p == ',')
        {
            if (count >= sizeof(fields) / sizeof(fields[0]))
                return GPS_NMEA_BAD_GGA;
            *p = '\0';
            fields[count++] = p + 1;
        }
    }
    if (count < 11U) return GPS_NMEA_BAD_GGA;

    GPS_NmeaFix_t next = {0};
    uint32_t quality, satellites;
    if (!ParseUnsigned(fields[6], 8U, &quality) ||
        !ParseUnsigned(fields[7], 255U, &satellites)) return GPS_NMEA_BAD_GGA;
    next.fix_quality = (uint8_t)quality;
    next.satellites_used = (uint8_t)satellites;

    // Accept GNSS fixes, including differential and RTK; exclude estimated fixes.
    if (quality == 1U || quality == 2U || quality == 4U || quality == 5U)
    {
        if (!ParseCoordinate(fields[2], fields[3], true, &next.latitude_e7) ||
            !ParseCoordinate(fields[4], fields[5], false, &next.longitude_e7))
            return GPS_NMEA_BAD_GGA;
        next.position_valid = true;

        int64_t value;
        if (fields[9][0] != '\0')
        {
            if (strcmp(fields[10], "M") != 0 ||
                !ParseScaled(fields[9], 1000U, &value) ||
                value < INT32_MIN || value > INT32_MAX) return GPS_NMEA_BAD_GGA;
            next.altitude_msl_mm = (int32_t)value;
            next.altitude_valid = true;
        }
        if (fields[8][0] != '\0')
        {
            if (!ParseScaled(fields[8], 100U, &value) ||
                fields[8][0] == '-' || value > UINT32_MAX)
                return GPS_NMEA_BAD_GGA;
            next.hdop_x100 = (uint32_t)value;
            next.hdop_valid = true;
        }
    }

    *out = next; // A valid no-fix sentence clears previously valid coordinates.
    return GPS_NMEA_GGA;
}

static void FormatScaled(char *buffer, size_t size, int32_t value,
                         uint32_t scale, unsigned int decimals)
{
    uint32_t magnitude = (uint32_t)(value < 0 ? -(int64_t)value : value);
    snprintf(buffer, size, "%s%lu.%0*lu", value < 0 ? "-" : "",
             (unsigned long)(magnitude / scale), (int)decimals,
             (unsigned long)(magnitude % scale));
}

int GPS_NmeaFormatFix(char *buffer, size_t size, const GPS_NmeaFix_t *fix)
{
    if (fix == NULL || !fix->position_valid)
        return snprintf(buffer, size, "GPS NO FIX Sat=%u FixQ=%u\r\n",
                        fix ? (unsigned int)fix->satellites_used : 0U,
                        fix ? (unsigned int)fix->fix_quality : 0U);

    char latitude[24], longitude[24], altitude[24];
    FormatScaled(latitude, sizeof(latitude), fix->latitude_e7, 10000000U, 7);
    FormatScaled(longitude, sizeof(longitude), fix->longitude_e7, 10000000U, 7);
    if (fix->altitude_valid)
        FormatScaled(altitude, sizeof(altitude), fix->altitude_msl_mm, 1000U, 3);
    else
        strcpy(altitude, "N/A");

    return snprintf(buffer, size,
        "GPS Lat=%s Lon=%s AltMSL=%s m Sat=%u FixQ=%u\r\n",
        latitude, longitude, altitude,
        (unsigned int)fix->satellites_used, (unsigned int)fix->fix_quality);
}
