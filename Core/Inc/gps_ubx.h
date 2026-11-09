#ifndef GPS_UBX_H
#define GPS_UBX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_UBX_PVT_SIZE 92U
#define GPS_UBX_WEEK_MS 604800000U

typedef struct
{
    uint32_t tow_ms;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_msl_mm;
    int32_t velocity_n_mm_s;
    int32_t velocity_e_mm_s;
    int32_t velocity_d_mm_s;
    int32_t ground_speed_mm_s;
    uint32_t horizontal_accuracy_mm;
    uint32_t vertical_accuracy_mm;
    uint32_t speed_accuracy_mm_s;
    int32_t nano;
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    uint8_t valid;
    uint8_t fix_type;
    uint8_t flags;
    uint8_t satellites;
    bool fix_ok;
} GPS_UbxPvt_t;

typedef struct
{
    uint32_t tow_ms;
    uint16_t hdop_x100;
    uint16_t vdop_x100;
} GPS_UbxDop_t;

typedef enum
{
    GPS_UBX_NONE,
    GPS_UBX_PVT,
    GPS_UBX_DOP,
    GPS_UBX_ACK,
    GPS_UBX_NAK,
    GPS_UBX_BAD_FRAME,
    GPS_UBX_BAD_PVT,
    GPS_UBX_BAD_DOP
} GPS_UbxEvent_t;

typedef struct
{
    uint8_t state, message_class, message_id, ck_a, ck_b, received_ck_a;
    uint16_t length, position;
    uint8_t payload[GPS_UBX_PVT_SIZE];
    GPS_UbxPvt_t pvt;
    GPS_UbxDop_t dop;
    uint8_t acknowledged_class, acknowledged_id;
} GPS_UbxParser_t;

void GPS_UbxReset(GPS_UbxParser_t *parser);
bool GPS_UbxBusy(const GPS_UbxParser_t *parser);
GPS_UbxEvent_t GPS_UbxFeed(GPS_UbxParser_t *parser, uint8_t byte);
bool GPS_UbxDecodePvt(const uint8_t *payload, size_t size, GPS_UbxPvt_t *out);

// M10 CFG-VALSET: RAM only, UART1 NAV-PVT + NAV-DOP at 10 Hz, NMEA off.
// Baud rate, input protocols, constellations, flash and backup RAM are untouched.
size_t GPS_UbxBuild10HzConfig(uint8_t *out, size_t capacity);
int GPS_UbxFormatPvt(char *out, size_t capacity, const GPS_UbxPvt_t *pvt);

#endif
