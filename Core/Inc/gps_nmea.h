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
    uint8_t satellites_used;
    uint8_t fix_quality;       // NMEA GGA quality; not a 2D/3D fix classification.
    bool position_valid;
    bool altitude_valid;
    bool hdop_valid;
} GPS_NmeaFix_t;

typedef enum
{
    GPS_NMEA_IGNORED,
    GPS_NMEA_GGA,
    GPS_NMEA_BAD_CHECKSUM,
    GPS_NMEA_BAD_GGA
} GPS_NmeaResult_t;

// Expects one null-terminated sentence without CR/LF. Updates out only on GGA.
GPS_NmeaResult_t GPS_NmeaParseGga(const char *line, GPS_NmeaFix_t *out);
int GPS_NmeaFormatFix(char *buffer, size_t size, const GPS_NmeaFix_t *fix);

#endif
