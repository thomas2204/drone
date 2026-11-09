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

static bool ParseUtcTime(const char *time, uint32_t *utc_ms)
{
    if (strcspn(time, ".") != 6U) return false;
    uint32_t hhmmss, scaled;
    char whole[7];
    memcpy(whole, time, 6U);
    whole[6] = '\0';
    int64_t value;
    if (!ParseUnsigned(whole, 235959U, &hhmmss) ||
        !ParseScaled(time, 1000U, &value) || value > UINT32_MAX) return false;
    scaled = (uint32_t)value;
    uint32_t hour = hhmmss / 10000U, minute = hhmmss / 100U % 100U;
    uint32_t second = hhmmss % 100U;
    if (minute >= 60U || second >= 60U) return false;
    *utc_ms = (hour * 3600U + minute * 60U + second) * 1000U + scaled % 1000U;
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
    if (*fields[1])
    {
        if (!ParseUtcTime(fields[1], &next.utc_ms)) return GPS_NMEA_BAD_GGA;
        next.time_valid = true;
    }
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

// Validates and splits a matching sentence, preserving empty columns.
// The caller supplies storage so no pointers survive beyond the parse call.
static GPS_NmeaResult_t SplitSentence(const char *line, const char *type,
    char *body, size_t body_size, char **fields, size_t *count,
    GPS_NmeaResult_t accepted, GPS_NmeaResult_t malformed)
{
    if (line == NULL || strlen(line) < 7U || line[0] != '$' ||
        memcmp(line + 3, type, 4) != 0) return GPS_NMEA_IGNORED;
    const char *star = strchr(line, '*');
    if (star == NULL || strlen(star) != 3U) return GPS_NMEA_BAD_CHECKSUM;
    int high = HexDigit(star[1]), low = HexDigit(star[2]);
    if (high < 0 || low < 0) return GPS_NMEA_BAD_CHECKSUM;
    uint8_t checksum = 0;
    for (const char *p = line + 1; p < star; ++p) checksum ^= (uint8_t)*p;
    if (checksum != (uint8_t)((high << 4) | low)) return GPS_NMEA_BAD_CHECKSUM;
    size_t length = (size_t)(star - line - 1);
    if (length >= body_size) return malformed;
    memcpy(body, line + 1, length);
    body[length] = '\0';
    *count = 1;
    fields[0] = body;
    for (char *p = body; *p; ++p)
    {
        if (*p == ',')
        {
            if (*count >= 20U) return malformed;
            *p = '\0';
            fields[(*count)++] = p + 1;
        }
    }
    return accepted;
}

static bool ParseNonnegativeScaled(const char *text, uint32_t scale,
                                   uint32_t max, uint32_t *out)
{
    int64_t value;
    if (*text == '-' || !ParseScaled(text, scale, &value) || value > max)
        return false;
    *out = (uint32_t)value;
    return true;
}

GPS_NmeaResult_t GPS_NmeaParseGsa(const char *line, GPS_NmeaGsa_t *out)
{
    if (out == NULL) return GPS_NMEA_IGNORED;
    char body[128], *fields[20];
    size_t count;
    GPS_NmeaResult_t result = SplitSentence(line, "GSA,", body, sizeof(body),
        fields, &count, GPS_NMEA_GSA, GPS_NMEA_BAD_GSA);
    if (result != GPS_NMEA_GSA) return result;
    uint32_t dimension;
    if (count < 18U ||
        (strcmp(fields[1], "A") != 0 && strcmp(fields[1], "M") != 0) ||
        !ParseUnsigned(fields[2], 3U, &dimension) || dimension == 0U)
        return GPS_NMEA_BAD_GSA;
    GPS_NmeaGsa_t next = {0};
    next.fix_dimension = (uint8_t)dimension;
    // DOP from no-fix sentences is not usable, even when 99.99 is supplied.
    if (dimension > 1U)
    {
        bool any = *fields[15] || *fields[16] || *fields[17];
        if (any)
        {
            if (!ParseNonnegativeScaled(fields[15], 100U, UINT32_MAX, &next.pdop_x100) ||
                !ParseNonnegativeScaled(fields[16], 100U, UINT32_MAX, &next.hdop_x100) ||
                !ParseNonnegativeScaled(fields[17], 100U, UINT32_MAX, &next.vdop_x100))
                return GPS_NMEA_BAD_GSA;
            next.dop_valid = true;
        }
    }
    *out = next;
    return GPS_NMEA_GSA;
}

static bool ParseUtcDate(const char *time, const char *date, GPS_NmeaRmc_t *out)
{
    if (strlen(date) != 6U || !ParseUtcTime(time, &out->utc_ms)) return false;
    uint32_t ddmmyy;
    if (!ParseUnsigned(date, 311299U, &ddmmyy)) return false;
    uint32_t day = ddmmyy / 10000U, month = ddmmyy / 100U % 100U;
    uint32_t year = 2000U + ddmmyy % 100U;
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 0U || month > 12U || day == 0U ||
        day > (uint32_t)days[month - 1U] + (month == 2U && year % 4U == 0U))
        return false;
    out->year = (uint16_t)year;
    out->month = (uint8_t)month;
    out->day = (uint8_t)day;
    out->time_valid = true;
    return true;
}

GPS_NmeaResult_t GPS_NmeaParseRmc(const char *line, GPS_NmeaRmc_t *out)
{
    if (out == NULL) return GPS_NMEA_IGNORED;
    char body[128], *fields[20];
    size_t count;
    GPS_NmeaResult_t result = SplitSentence(line, "RMC,", body, sizeof(body),
        fields, &count, GPS_NMEA_RMC, GPS_NMEA_BAD_RMC);
    if (result != GPS_NMEA_RMC) return result;
    if (count < 10U ||
        (strcmp(fields[2], "A") != 0 && strcmp(fields[2], "V") != 0))
        return GPS_NMEA_BAD_RMC;
    GPS_NmeaRmc_t next = {0};
    if (*fields[1] && *fields[9] && !ParseUtcDate(fields[1], fields[9], &next))
        return GPS_NMEA_BAD_RMC;
    // A valid no-fix sentence clears old position and velocity immediately.
    if (strcmp(fields[2], "A") == 0)
    {
        if (!ParseCoordinate(fields[3], fields[4], true, &next.latitude_e7) ||
            !ParseCoordinate(fields[5], fields[6], false, &next.longitude_e7))
            return GPS_NMEA_BAD_RMC;
        next.position_valid = true;
        if (*fields[7])
        {
            uint32_t knots_x1000;
            if (!ParseNonnegativeScaled(fields[7], 1000U, UINT32_MAX, &knots_x1000))
                return GPS_NMEA_BAD_RMC;
            // One knot = 1852/3600 m/s. Round to mm/s.
            next.speed_mm_s = (uint32_t)(((uint64_t)knots_x1000 * 1852U + 1800U) / 3600U);
            next.speed_valid = true;
        }
        if (*fields[8])
        {
            int64_t degrees_e7;
            if (*fields[8] == '-' || !ParseScaled(fields[8], 10000000U, &degrees_e7) ||
                degrees_e7 >= 3600000000LL) return GPS_NMEA_BAD_RMC;
            next.course_cdeg = (uint32_t)(degrees_e7 / 100000);
            next.course_valid = true;
        }
    }
    *out = next;
    return GPS_NMEA_RMC;
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
