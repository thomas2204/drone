#ifndef GPS_RAW_H
#define GPS_RAW_H

#include <stdbool.h>
#include <stdint.h>
#include "gps_nmea.h"

#define GPS_RAW_SAMPLE_SIZE 24U
#define GPS_RAW_LINE_SIZE 128U

typedef struct
{
    uint32_t rx_bytes;  //byte sent , fro debug
    uint32_t starts;   //Number of successful DMA receive starts , debug
    uint32_t last_uart_error; //debug
    uint32_t nmea_lines;
    uint32_t ubx_markers;
    uint32_t last_line_ms;
    uint16_t sample_size;
    uint8_t sample[GPS_RAW_SAMPLE_SIZE];
    char line[GPS_RAW_LINE_SIZE];
    GPS_NmeaFix_t fix;
    uint32_t gga_count;
    uint32_t last_gga_ms;
    uint32_t gga_errors;
} GPS_RawSnapshot_t;

void GPS_RawInit(void);
void GPS_RawTask(void);
bool GPS_RawGetSnapshot(GPS_RawSnapshot_t *out);

#endif
