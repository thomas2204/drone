#include "gps_nmea.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static GPS_NmeaResult_t ParseBody(const char *body, GPS_NmeaFix_t *fix)
{
    unsigned int checksum = 0;
    for (const char *p = body; *p; ++p) checksum ^= (unsigned char)*p;
    char sentence[256];
    snprintf(sentence, sizeof(sentence), "$%s*%02X", body, checksum);
    return GPS_NmeaParseGga(sentence, fix);
}

int main(void)
{
    GPS_NmeaFix_t fix = {0};
    char text[192];

    // Known NMEA example with an independently specified checksum.
    assert(GPS_NmeaParseGga(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
        &fix) == GPS_NMEA_GGA);
    assert(fix.position_valid && fix.altitude_valid && fix.hdop_valid);
    assert(fix.latitude_e7 == 481173000 && fix.longitude_e7 == 115166667);
    assert(fix.altitude_msl_mm == 545400 && fix.hdop_x100 == 90);
    assert(fix.satellites_used == 8 && fix.fix_quality == 1);
    GPS_NmeaFormatFix(text, sizeof(text), &fix);
    assert(strcmp(text,
        "GPS Lat=48.1173000 Lon=11.5166667 AltMSL=545.400 m Sat=8 FixQ=1\r\n") == 0);

    // A corrupt packet must not replace the last accepted measurement.
    GPS_NmeaFix_t saved = fix;
    assert(GPS_NmeaParseGga(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00",
        &fix) == GPS_NMEA_BAD_CHECKSUM);
    assert(memcmp(&fix, &saved, sizeof(fix)) == 0);
    assert(GPS_NmeaParseGga("$GNGGA,1*", &fix) == GPS_NMEA_BAD_CHECKSUM);
    assert(GPS_NmeaParseGga("$GNGGA,1*ZZ", &fix) == GPS_NMEA_BAD_CHECKSUM);

    // Empty fields must stay in their original column positions.
    assert(ParseBody("GNGGA,,,,,,0,00,99.99,,,,,,", &fix) == GPS_NMEA_GGA);
    assert(!fix.position_valid && !fix.altitude_valid && fix.satellites_used == 0);
    assert(fix.latitude_e7 == 0 && fix.longitude_e7 == 0);
    GPS_NmeaFormatFix(text, sizeof(text), &fix);
    assert(strcmp(text, "GPS NO FIX Sat=0 FixQ=0\r\n") == 0);

    // Southern/western hemispheres and below-sea-level altitude.
    assert(ParseBody("GNGGA,010203,3456.789,S,05822.123,W,2,12,1.20,-12.345,M,0,M,,",
                     &fix) == GPS_NMEA_GGA);
    assert(fix.latitude_e7 == -349464833 && fix.longitude_e7 == -583687167);
    assert(fix.altitude_msl_mm == -12345 && fix.hdop_x100 == 120);

    // Preserve the sign when magnitude is less than one degree/metre.
    assert(ParseBody("GNGGA,010203,0000.006,S,00000.006,W,1,04,1,-0.125,M,0,M,,",
                     &fix) == GPS_NMEA_GGA);
    GPS_NmeaFormatFix(text, sizeof(text), &fix);
    assert(strstr(text, "Lat=-0.0001000 Lon=-0.0001000 AltMSL=-0.125") != NULL);

    // Valid limits and optional altitude/HDOP fields.
    assert(ParseBody("GNGGA,010203,9000.000,N,18000.000,E,1,10,,,,,,,",
                     &fix) == GPS_NMEA_GGA);
    assert(fix.latitude_e7 == 900000000 && fix.longitude_e7 == 1800000000);
    assert(!fix.altitude_valid && !fix.hdop_valid);
    GPS_NmeaFormatFix(text, sizeof(text), &fix);
    assert(strstr(text, "AltMSL=N/A") != NULL);

    const char *bad[] = {
        "GNGGA,1,4860.000,N,01131.000,E,1,8,1,10,M",
        "GNGGA,1,9000.001,N,01131.000,E,1,8,1,10,M",
        "GNGGA,1,4807.038,N,18000.001,E,1,8,1,10,M",
        "GNGGA,1,4807.038,X,01131.000,E,1,8,1,10,M",
        "GNGGA,1,,N,01131.000,E,1,8,1,10,M",
        "GNGGA,1,4807.038,N,01131.000,E,1,8,1,10,F",
        "GNGGA,1,4807.038,N,01131.000,E,1,8,-1,10,M",
        "GNGGA,1,4807.038,N,01131.000,E,1,256,1,10,M",
        "GNGGA,1,4807.038,N,01131.000,E,1,8,1,2147484,M",
        "GNGGA,1,4807.038,N,01131.000,E,1,8,1,1e3,M",
        "GNGGA,1,4807.038,N,01131.000,E,1,8,1,999999999999999999999,M",
        "GNGGA,1,4807.038,N,01131.000,E,1,8"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(ParseBody(bad[i], &fix) == GPS_NMEA_BAD_GGA);

    assert(ParseBody("GNGGA,1,4807.038,N,01131.000,E,6,8,1,10,M", &fix)
           == GPS_NMEA_GGA);
    assert(!fix.position_valid); // Estimated/dead-reckoning is not a GNSS fix.
    assert(GPS_NmeaParseGga("$GNGLL,,,,,,V,N*7A", &fix) == GPS_NMEA_IGNORED);
    assert(GPS_NmeaParseGga("", &fix) == GPS_NMEA_IGNORED);
    assert(GPS_NmeaParseGga(NULL, &fix) == GPS_NMEA_IGNORED);
    assert(ParseBody("GNGGA,,,,,,0,00,99.99,,,,,,", NULL) == GPS_NMEA_IGNORED);

    // snprintf must respect short caller buffers, including the terminator.
    char short_text[5];
    assert(GPS_NmeaFormatFix(short_text, sizeof(short_text), &fix) > 4);
    assert(short_text[4] == '\0');
    puts("GPS NMEA tests passed: fixes, no-fix, checksum, ranges, and formatting.");
    return 0;
}
