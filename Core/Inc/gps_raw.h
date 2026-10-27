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
    // Latest raw sentences for inspecting fields before GPS_INPUT forwarding.
    char gsa_line[GPS_RAW_LINE_SIZE];
    char rmc_line[GPS_RAW_LINE_SIZE];
    uint32_t gsa_count;
    uint32_t rmc_count;
    GPS_NmeaFix_t fix;
    uint32_t gga_count;
    uint32_t last_gga_ms;
    uint32_t gga_errors;
    GPS_NmeaGsa_t gsa;
    GPS_NmeaRmc_t rmc;
    uint32_t gsa_decoded_count;
    uint32_t rmc_decoded_count;
    uint32_t last_gsa_ms;
    uint32_t last_rmc_ms;
    uint32_t gsa_errors;
    uint32_t rmc_errors;
} GPS_RawSnapshot_t;

void GPS_RawInit(void);
void GPS_RawTask(void);
bool GPS_RawGetSnapshot(GPS_RawSnapshot_t *out);

#endif
