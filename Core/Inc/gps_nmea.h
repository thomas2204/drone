#ifndef GPS_NMEA_H
#define GPS_NMEA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int32_t latitude_e7;       // Decimal degrees multiplied by 10^7.
    int32_t longitude_e7;
    int32_t altitude_msl_mm;   // Mean sea level, not height above the ground.
    uint32_t hdop_x100;
    uint32_t utc_ms;
    uint8_t satellites_used;
    uint8_t fix_quality;       // NMEA GGA quality; not a 2D/3D fix classification.
    bool position_valid;
    bool altitude_valid;
    bool hdop_valid;
    bool time_valid;
} GPS_NmeaFix_t;

typedef struct
{
    uint8_t fix_dimension;    // 1=no fix, 2=2D, 3=3D; independent of GGA quality.
    uint32_t pdop_x100;
    uint32_t hdop_x100;
    uint32_t vdop_x100;
    bool dop_valid;
} GPS_NmeaGsa_t;

typedef struct
{
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint32_t utc_ms;          // Milliseconds since UTC midnight.
    uint32_t speed_mm_s;
    uint32_t course_cdeg;     // Direction of travel, not vehicle yaw.
    uint16_t year;            // RMC two-digit year interpreted as 2000..2099.
    uint8_t month;
    uint8_t day;
    bool position_valid;
    bool time_valid;
    bool speed_valid;
    bool course_valid;
} GPS_NmeaRmc_t;

typedef enum
{
    GPS_NMEA_IGNORED,
    GPS_NMEA_GGA,
    GPS_NMEA_BAD_CHECKSUM,
    GPS_NMEA_BAD_GGA,
    GPS_NMEA_GSA,
    GPS_NMEA_RMC,
    GPS_NMEA_BAD_GSA,
    GPS_NMEA_BAD_RMC
} GPS_NmeaResult_t;

// Expects one null-terminated sentence without CR/LF. Updates out only on GGA.
GPS_NmeaResult_t GPS_NmeaParseGga(const char *line, GPS_NmeaFix_t *out);
GPS_NmeaResult_t GPS_NmeaParseGsa(const char *line, GPS_NmeaGsa_t *out);
GPS_NmeaResult_t GPS_NmeaParseRmc(const char *line, GPS_NmeaRmc_t *out);
int GPS_NmeaFormatFix(char *buffer, size_t size, const GPS_NmeaFix_t *fix);

#endif
