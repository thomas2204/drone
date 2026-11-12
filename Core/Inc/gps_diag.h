#ifndef GPS_DIAG_H
#define GPS_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifndef GPS_DIAGNOSTICS_ENABLED
#define GPS_DIAGNOSTICS_ENABLED 0
#endif

#define GPS_DIAG_KEYS 14U
#define GPS_DIAG_FILTERS 22U
#define GPS_DIAG_POLLS (17U + GPS_DIAG_FILTERS)
#define GPS_DIAG_BASE_LINES 17U
#define GPS_DIAG_TOP_SATS 8U
#define GPS_DIAG_MAX_PAYLOAD 3068U

typedef struct {
    uint8_t gnss, sv, cno, quality, used;
    uint8_t health, orbit, ephemeris, almanac;
} GPS_DiagSatellite_t;

typedef struct {
    uint32_t polls, tx_errors, bad_frames, replies, naks;
    uint32_t version_count, config_count, sat_count, rf_count, last_sat_ms;
    char software[31], hardware[11], firmware[31], protocol[31], module[31];
    uint16_t config_known, config_enabled;
    uint32_t filter_known, filters[GPS_DIAG_FILTERS];
    uint8_t listed, acquired, locked, used, max_cno, top_count;
    uint8_t constellation_listed[7], constellation_used[7];
    uint8_t constellation_locked[7], constellation_ephemeris[7], constellation_unhealthy[7];
    GPS_DiagSatellite_t top[GPS_DIAG_TOP_SATS];
    uint8_t jam_state, antenna_status, antenna_power, cw_suppression;
    uint16_t noise, agc;
} GPS_DiagSnapshot_t;

/* Static storage: this parser must not be put on a small RTOS task stack. */
typedef struct {
    uint8_t state, cls, id, a, b, received_a;
    uint16_t length, pos;
    uint8_t payload[GPS_DIAG_MAX_PAYLOAD];
} GPS_DiagParser_t;

void GPS_DiagReset(GPS_DiagParser_t *parser);
void GPS_DiagFeed(GPS_DiagParser_t *parser, uint8_t byte,
                  GPS_DiagSnapshot_t *snapshot, uint32_t now);
/* Only MON-VER, CFG-VALGET (RAM), NAV-SAT and MON-RF polls can be built. */
size_t GPS_DiagBuildPoll(unsigned slot, uint8_t *out, size_t capacity);
/* Returns zero after the last line. Unknown configuration is printed as '?'. */
int GPS_DiagFormatLine(const GPS_DiagSnapshot_t *s, unsigned line,
                       uint32_t now, char *out, size_t capacity);
#endif
