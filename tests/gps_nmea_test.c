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

static GPS_NmeaResult_t ParseNavigationBody(const char *body, void *out, bool gsa)
{
    unsigned int checksum = 0;
    for (const char *p = body; *p; ++p) checksum ^= (unsigned char)*p;
    char sentence[256];
    snprintf(sentence, sizeof(sentence), "$%s*%02X", body, checksum);
    return gsa ? GPS_NmeaParseGsa(sentence, out) : GPS_NmeaParseRmc(sentence, out);
}

static void TestNavigation(void)
{
    GPS_NmeaGsa_t gsa = {0};
    GPS_NmeaRmc_t rmc = {0};
    // Receiver screenshot: independent checksum, empty course is unavailable.
    assert(GPS_NmeaParseRmc(
        "$GNRMC,084841.00,A,2220.17850,N,11416.08404,E,0.120,,271026,,,A,V*14",
        &rmc) == GPS_NMEA_RMC);
    assert(rmc.position_valid && rmc.time_valid && rmc.speed_valid && !rmc.course_valid);
    assert(rmc.utc_ms == 31721000U && rmc.year == 2026 && rmc.month == 10 && rmc.day == 27);
    assert(rmc.latitude_e7 == 223363083 && rmc.longitude_e7 == 1142680673);
    assert(rmc.speed_mm_s == 62U);
    assert(ParseNavigationBody("GNGSA,A,3,02,,,,,,,,,,,,3.33,2.73,1.92,5", &gsa, true)
           == GPS_NMEA_GSA);
    assert(gsa.fix_dimension == 3 && gsa.dop_valid);
    assert(gsa.pdop_x100 == 333 && gsa.hdop_x100 == 273 && gsa.vdop_x100 == 192);
    GPS_NmeaGsa_t saved_gsa = gsa;
    assert(GPS_NmeaParseGsa("$GNGSA,A,3*00", &gsa) == GPS_NMEA_BAD_CHECKSUM);
    assert(memcmp(&gsa, &saved_gsa, sizeof(gsa)) == 0);
    assert(ParseNavigationBody("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,5", &gsa, true)
           == GPS_NMEA_GSA);
    assert(gsa.fix_dimension == 1 && !gsa.dop_valid);
    assert(ParseNavigationBody("GPGSA,M,2,02,,,,,,,,,,,,2.0,1.0,3.0", &gsa, true)
           == GPS_NMEA_GSA);
    assert(gsa.fix_dimension == 2);
    assert(ParseNavigationBody("GNGSA,A,4,02,,,,,,,,,,,,3,2,1", &gsa, true)
           == GPS_NMEA_BAD_GSA);
    assert(ParseNavigationBody("GNGSA,A,3,02,,,,,,,,,,,,3,-2,1", &gsa, true)
           == GPS_NMEA_BAD_GSA);
    assert(ParseNavigationBody("GNRMC,235959.999,A,3456.789,S,05822.123,W,1.000,359.99,290224,,,A,V",
           &rmc, false) == GPS_NMEA_RMC);
    assert(rmc.utc_ms == 86399999U && rmc.speed_mm_s == 514U);
    assert(rmc.course_valid && rmc.course_cdeg == 35999U);
    assert(rmc.latitude_e7 < 0 && rmc.longitude_e7 < 0);
    GPS_NmeaRmc_t saved_rmc = rmc;
    const char *bad_rmc[] = {
        "GNRMC,240000,A,2220.1,N,11416.1,E,0,,271026",
        "GNRMC,126000,A,2220.1,N,11416.1,E,0,,271026",
        "GNRMC,125960,A,2220.1,N,11416.1,E,0,,271026",
        "GNRMC,120000,A,2220.1,N,11416.1,E,0,,290223",
        "GNRMC,120000,A,2220.1,N,11416.1,E,0,,310426",
        "GNRMC,120000,A,2220.1,N,11416.1,E,0,,000026",
        "GNRMC,120000,A,2220.1,N,11416.1,E,-1,,271026",
        "GNRMC,120000,A,2220.1,N,11416.1,E,0,360,271026",
        "GNRMC,120000,A,2220.1,N,11416.1,E,0,-0.1,271026",
        "GNRMC,120000,A,2260.1,N,11416.1,E,0,,271026",
        "GNRMC,120000,X,2220.1,N,11416.1,E,0,,271026"
    };
    for (size_t i = 0; i < sizeof(bad_rmc) / sizeof(bad_rmc[0]); ++i)
    {
        assert(ParseNavigationBody(bad_rmc[i], &rmc, false) == GPS_NMEA_BAD_RMC);
        assert(memcmp(&rmc, &saved_rmc, sizeof(rmc)) == 0);
    }
    assert(GPS_NmeaParseRmc("$GNRMC,120000*ZZ", &rmc) == GPS_NMEA_BAD_CHECKSUM);
    assert(ParseNavigationBody("GNRMC,120000,V,,,,,,,271026,,,N,V", &rmc, false) == GPS_NMEA_RMC);
    assert(!rmc.position_valid && !rmc.speed_valid && !rmc.course_valid && rmc.time_valid);
    assert(ParseNavigationBody("GNRMC,,V,,,,,,,,,,N,V", &rmc, false) == GPS_NMEA_RMC);
    assert(!rmc.time_valid);
    assert(GPS_NmeaParseGsa(NULL, &gsa) == GPS_NMEA_IGNORED);
    assert(GPS_NmeaParseRmc(NULL, &rmc) == GPS_NMEA_IGNORED);
    assert(GPS_NmeaParseGsa("", &gsa) == GPS_NMEA_IGNORED);
    assert(GPS_NmeaParseRmc("", &rmc) == GPS_NMEA_IGNORED);
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
    assert(fix.time_valid && fix.utc_ms == 45319000U);
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
        "GNGGA,010203,4860.000,N,01131.000,E,1,8,1,10,M",
        "GNGGA,010203,9000.001,N,01131.000,E,1,8,1,10,M",
        "GNGGA,010203,4807.038,N,18000.001,E,1,8,1,10,M",
        "GNGGA,010203,4807.038,X,01131.000,E,1,8,1,10,M",
        "GNGGA,010203,,N,01131.000,E,1,8,1,10,M",
        "GNGGA,010203,4807.038,N,01131.000,E,1,8,1,10,F",
        "GNGGA,010203,4807.038,N,01131.000,E,1,8,-1,10,M",
        "GNGGA,010203,4807.038,N,01131.000,E,1,256,1,10,M",
        "GNGGA,010203,4807.038,N,01131.000,E,1,8,1,2147484,M",
        "GNGGA,010203,4807.038,N,01131.000,E,1,8,1,1e3,M",
        "GNGGA,010203,4807.038,N,01131.000,E,1,8,1,999999999999999999999,M",
        "GNGGA,010203,4807.038,N,01131.000,E,1,8"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(ParseBody(bad[i], &fix) == GPS_NMEA_BAD_GGA);

    assert(ParseBody("GNGGA,010203,4807.038,N,01131.000,E,6,8,1,10,M", &fix)
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
    TestNavigation();
    puts("GPS NMEA tests passed: GGA/GSA/RMC, no-fix, checksums, dates, units, and formatting.");
    return 0;
}
