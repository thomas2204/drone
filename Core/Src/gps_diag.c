#include "gps_diag.h"
#include <stdio.h>
#include <string.h>

/* u-blox M10 SPG 5.10 UBX-21035062: CFG-SIGNAL master and signal keys. */
static const uint32_t keys[GPS_DIAG_KEYS] = {
    0x1031001f, 0x10310020, 0x10310021, 0x10310022, 0x10310024, 0x10310025,
    0x10310001, 0x10310005, 0x10310007, 0x1031000d, 0x1031000f,
    0x10310012, 0x10310014, 0x10310018
};

/* RAM keys, UBX-21035062 R03 tables 7, 20 and 33. Storage width is
 * encoded in each key. MINELEV is signed I1. No settings are written. */
static const uint32_t filter_keys[GPS_DIAG_FILTERS] = {
    0x20110021, 0x20110011, 0x201100a1, 0x201100a2,
    0x201100a3, 0x201100a4, 0x201100aa, 0x201100ab,
    0x301100b1, 0x301100b2, 0x301100b3, 0x10110013,
    0x20d00001, 0x10d0000a, 0x10d00010, 0x20d00006,
    0x20d00007, 0x30d00005, 0x40d00002, 0x40d00003,
    0x10340014, 0x201100d6
};

static uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t U32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void Text(char *out, size_t capacity, const uint8_t *p, size_t size) {
    size_t i = 0;
    while (i < size && i + 1U < capacity && p[i] != 0U) {
        out[i] = p[i] >= 32U && p[i] <= 126U ? (char)p[i] : '?';
        ++i;
    }
    out[i] = 0;
}

void GPS_DiagReset(GPS_DiagParser_t *p) { p->state = 0; }

size_t GPS_DiagBuildPoll(unsigned slot, uint8_t *out, size_t capacity) {
    if (!out || slot >= GPS_DIAG_POLLS) return 0;
    size_t n = ((slot >= 1U && slot <= GPS_DIAG_KEYS) || slot >= 17U) ? 8U : 0U;
    if (capacity < n + 8U) return 0;
    memset(out, 0, n + 8U);
    out[0] = 0xb5; out[1] = 0x62;
    if (slot == 0U) { out[2] = 0x0a; out[3] = 0x04; }
    else if (slot <= GPS_DIAG_KEYS || slot >= 17U) {
        out[2] = 6; out[3] = 0x8b; out[4] = 8;
        /* version=0, layer=0 (RAM), position=0; one key per request. */
        uint32_t key = slot >= 17U ? filter_keys[slot - 17U] : keys[slot - 1U];
        for (unsigned i = 0; i < 4; ++i) out[10U + i] = (uint8_t)(key >> (8U * i));
    } else if (slot == 15U) { out[2] = 1; out[3] = 0x35; }
    else { out[2] = 0x0a; out[3] = 0x38; }
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < n + 6U; ++i) { a += out[i]; b += a; }
    out[n + 6U] = a; out[n + 7U] = b;
    return n + 8U;
}

static void Decode(GPS_DiagParser_t *p, GPS_DiagSnapshot_t *s, uint32_t now) {
    const uint8_t *v = p->payload;
    unsigned n = p->length;
    if (p->cls == 0x0a && p->id == 4) {
        if (n < 40U || (n - 40U) % 30U != 0U) { s->bad_frames++; return; }
        Text(s->software, sizeof(s->software), v, 30);
        Text(s->hardware, sizeof(s->hardware), v + 30, 10);
        s->firmware[0] = s->protocol[0] = s->module[0] = 0;
        for (unsigned i = 40; i < n; i += 30U) {
            if (memcmp(v + i, "FWVER=", 6) == 0)
                Text(s->firmware, sizeof(s->firmware), v + i + 6, 24);
            else if (memcmp(v + i, "PROTVER=", 8) == 0)
                Text(s->protocol, sizeof(s->protocol), v + i + 8, 22);
            else if (memcmp(v + i, "MOD=", 4) == 0)
                Text(s->module, sizeof(s->module), v + i + 4, 26);
        }
        s->version_count++; s->replies++;
    } else if (p->cls == 6 && p->id == 0x8b) {
        /* Exactly one requested key and its correctly sized value per reply. */
        if (n < 9U || v[0] != 1U || v[1] != 0U || U16(v + 2) != 0U) {
            s->bad_frames++; return;
        }
        uint32_t key = U32(v + 4);
        for (unsigned i = 0; i < GPS_DIAG_FILTERS; ++i) {
            if (key != filter_keys[i]) continue;
            unsigned type = key >> 28;
            unsigned width = type <= 2U ? 1U : type == 3U ? 2U : 4U;
            if (n != 8U + width || (type == 1U && v[8] > 1U)) {
                s->bad_frames++; return;
            }
            s->filters[i] = width == 4U ? U32(v + 8) : width == 2U ? U16(v + 8) : v[8];
            s->filter_known |= 1UL << i;
            s->config_count++; s->replies++; return;
        }
        for (unsigned i = 0; i < GPS_DIAG_KEYS; ++i) {
            if (key != keys[i]) continue;
            if (n != 9U || v[8] > 1U) { s->bad_frames++; return; }
            uint16_t bit = (uint16_t)(1U << i);
            s->config_known |= bit;
            s->config_enabled = (uint16_t)((s->config_enabled & ~bit) | (v[8] ? bit : 0U));
            s->config_count++; s->replies++; break;
        }
    } else if (p->cls == 1 && p->id == 0x35) {
        if (n < 8U || v[4] != 1U || n != 8U + 12U * v[5]) { s->bad_frames++; return; }
        s->listed = v[5]; s->acquired = s->locked = s->used = s->max_cno = s->top_count = 0;
        memset(s->constellation_listed, 0, sizeof(s->constellation_listed));
        memset(s->constellation_used, 0, sizeof(s->constellation_used));
        memset(s->constellation_locked, 0, sizeof(s->constellation_locked));
        memset(s->constellation_ephemeris, 0, sizeof(s->constellation_ephemeris));
        memset(s->constellation_unhealthy, 0, sizeof(s->constellation_unhealthy));
        memset(s->top, 0, sizeof(s->top));
        for (unsigned i = 8; i < n; i += 12U) {
            uint32_t flags = U32(v + i + 8U);
            GPS_DiagSatellite_t sat = {
                v[i], v[i+1], v[i+2], (uint8_t)(flags & 7U), (uint8_t)((flags >> 3) & 1U),
                (uint8_t)((flags >> 4) & 3U), (uint8_t)((flags >> 8) & 7U),
                (uint8_t)((flags >> 11) & 1U), (uint8_t)((flags >> 12) & 1U)
            };
            if (sat.quality >= 2U) s->acquired++;
            if (sat.quality >= 4U) s->locked++;
            s->used += sat.used;
            if (sat.cno > s->max_cno) s->max_cno = sat.cno;
            if (sat.gnss < 7U) {
                s->constellation_listed[sat.gnss]++;
                s->constellation_used[sat.gnss] += sat.used;
                s->constellation_locked[sat.gnss] += sat.quality >= 4U;
                s->constellation_ephemeris[sat.gnss] += sat.ephemeris;
                s->constellation_unhealthy[sat.gnss] += sat.health == 2U;
            }
            unsigned at = 0;
            while (at < s->top_count && s->top[at].cno >= sat.cno) ++at;
            if (at < GPS_DIAG_TOP_SATS) {
                unsigned end = s->top_count < GPS_DIAG_TOP_SATS ? s->top_count : GPS_DIAG_TOP_SATS - 1U;
                while (end > at) { s->top[end] = s->top[end-1U]; --end; }
                s->top[at] = sat;
                if (s->top_count < GPS_DIAG_TOP_SATS) s->top_count++;
            }
        }
        s->sat_count++; s->last_sat_ms = now; s->replies++;
    } else if (p->cls == 0x0a && p->id == 0x38) {
        if (n < 4U || v[0] != 0U || n != 4U + 24U * v[1]) { s->bad_frames++; return; }
        for (unsigned i = 4; i < n; i += 24U) {
            if (v[i] != 0U) continue; /* L1 RF block only */
            s->jam_state = v[i+1] & 3U; s->antenna_status = v[i+2]; s->antenna_power = v[i+3];
            s->noise = U16(v+i+12); s->agc = U16(v+i+14); s->cw_suppression = v[i+16];
            s->rf_count++; s->replies++; break;
        }
    } else if (p->cls == 5 && p->id == 0 && n == 2U && v[0] == 6 && v[1] == 0x8b) s->naks++;
}

void GPS_DiagFeed(GPS_DiagParser_t *p, uint8_t byte, GPS_DiagSnapshot_t *s, uint32_t now) {
    switch (p->state) {
    case 0: if (byte == 0xb5) p->state = 1; return;
    case 1: p->state = byte == 0x62 ? 2 : (byte == 0xb5 ? 1 : 0); p->a = p->b = 0; return;
    case 2: p->cls = byte; p->state = 3; break;
    case 3: p->id = byte; p->state = 4; break;
    case 4: p->length = byte; p->state = 5; break;
    case 5:
        p->length |= (uint16_t)byte << 8; p->pos = 0;
        if (p->length > sizeof(p->payload)) { GPS_DiagReset(p); s->bad_frames++; return; }
        p->state = p->length ? 6 : 7; break;
    case 6: p->payload[p->pos++] = byte; if (p->pos == p->length) p->state = 7; break;
    case 7: p->received_a = byte; p->state = 8; return;
    case 8:
        GPS_DiagReset(p);
        if (p->received_a == p->a && byte == p->b) Decode(p, s, now);
        else { s->bad_frames++; if (byte == 0xb5) p->state = 1; }
        return;
    default: GPS_DiagReset(p); return;
    }
    p->a += byte; p->b += p->a;
}

static char Setting(const GPS_DiagSnapshot_t *s, unsigned i) {
    return !(s->config_known & (1U << i)) ? '?' : (s->config_enabled & (1U << i) ? '1' : '0');
}
static const char *Filter(const GPS_DiagSnapshot_t *s, unsigned i, char out[12]) {
    if (!(s->filter_known & (1U << i))) return "?";
    if (i == 5U) {
        int value = (int)s->filters[i];
        if (value >= 128) value -= 256; /* signed elevation */
        snprintf(out, 12, "%d", value);
    } else snprintf(out, 12, "%lu", (unsigned long)s->filters[i]);
    return out;
}
int GPS_DiagFormatLine(const GPS_DiagSnapshot_t *s, unsigned line, uint32_t now, char *out, size_t cap) {
    static const char *names[7] = {"GPS", "SBAS", "GAL", "BDS", "IMES", "QZSS", "GLO"};
    char f[4][12];
    switch (line) {
    case 0: return snprintf(out, cap, "GPS READ polls=%lu replies=%lu txerr=%lu bad=%lu nak=%lu\r\n",
        (unsigned long)s->polls, (unsigned long)s->replies, (unsigned long)s->tx_errors,
        (unsigned long)s->bad_frames, (unsigned long)s->naks);
    case 1: return snprintf(out, cap, "GPS ID SW=%s HW=%s FW=%s PROT=%s MOD=%s\r\n",
        s->software, s->hardware, s->firmware, s->protocol, s->module);
    case 2: return snprintf(out, cap, "GPS CFG RAM GPS=%c SBAS=%c GAL=%c BDS=%c QZSS=%c GLO=%c (?=unknown)\r\n",
        Setting(s,0), Setting(s,1), Setting(s,2), Setting(s,3), Setting(s,4), Setting(s,5));
    case 3: return snprintf(out, cap, "GPS SIGNAL GPS-L1=%c SBAS-L1=%c GAL-E1=%c BDS-B1I=%c BDS-B1C=%c QZ-L1CA=%c QZ-L1S=%c GLO-L1=%c\r\n",
        Setting(s,6), Setting(s,7), Setting(s,8), Setting(s,9), Setting(s,10), Setting(s,11), Setting(s,12), Setting(s,13));
    case 4:
        if (!s->sat_count) return snprintf(out, cap, "GPS SAT: no NAV-SAT reply yet\r\n");
        return snprintf(out, cap, "GPS SAT listed=%u acquired=%u locked=%u used=%u maxCNO=%u dBHz age=%lums\r\n",
            s->listed, s->acquired, s->locked, s->used, s->max_cno, (unsigned long)(now-s->last_sat_ms));
    case 5: return snprintf(out, cap, "GPS SAT listed/used GPS=%u/%u GAL=%u/%u BDS=%u/%u GLO=%u/%u QZSS=%u/%u SBAS=%u/%u\r\n",
        s->constellation_listed[0],s->constellation_used[0],s->constellation_listed[2],s->constellation_used[2],
        s->constellation_listed[3],s->constellation_used[3],s->constellation_listed[6],s->constellation_used[6],
        s->constellation_listed[5],s->constellation_used[5],s->constellation_listed[1],s->constellation_used[1]);
    case 6:
        if (!s->rf_count) return snprintf(out, cap, "GPS RF: no MON-RF L1 reply yet\r\n");
        return snprintf(out, cap, "GPS RF jam=%u ant=%u power=%u noise=%u agc=%u CW=%u (jam0=unknown)\r\n",
            s->jam_state,s->antenna_status,s->antenna_power,s->noise,s->agc,s->cw_suppression);
    case 7: return snprintf(out, cap, "GPS NAV RAM dyn=%s fixMode=%s minSV=%s maxSV=%s\r\n",
        Filter(s,0,f[0]),Filter(s,1,f[1]),Filter(s,2,f[2]),Filter(s,3,f[3]));
    case 8: return snprintf(out, cap, "GPS FILTER RAM minCNO=%s dBHz minElev=%s deg nCNO=%s CNOthr=%s dBHz\r\n",
        Filter(s,4,f[0]),Filter(s,5,f[1]),Filter(s,6,f[2]),Filter(s,7,f[3]));
    case 9: return snprintf(out, cap, "GPS OUTPUT RAM pDOPx10=%s tDOPx10=%s pAcc=%s m ini3D=%s\r\n",
        Filter(s,8,f[0]),Filter(s,9,f[1]),Filter(s,10,f[2]),Filter(s,11,f[3]));
    case 10: case 11: case 12: case 13: {
        static const unsigned ids[] = {0,2,3,5};
        unsigned g = ids[line-10U];
        return snprintf(out, cap, "GPS HEALTH %s locked=%u eph=%u unhealthy=%u (all listed SVs)\r\n",
            names[g],s->constellation_locked[g],s->constellation_ephemeris[g],s->constellation_unhealthy[g]);
    }
    case 14: return snprintf(out, cap, "GPS POWER RAM mode=%s updateEph=%s limitPeak=%s (mode0=full 1=on/off 2=cyclic)\r\n",
        Filter(s,12,f[0]),Filter(s,13,f[1]),Filter(s,14,f[2]));
    case 15: return snprintf(out, cap, "GPS POWER RAM minAcq=%ss maxAcq=%ss onTime=%ss updatePeriod=%ss\r\n",
        Filter(s,15,f[0]),Filter(s,16,f[1]),Filter(s,17,f[2]),Filter(s,18,f[3]));
    case 16: return snprintf(out, cap, "GPS EXTRA RAM acqPeriod=%ss BDS-GEO=%s sigAttComp=%s (0=off 255=auto)\r\n",
        Filter(s,19,f[0]),Filter(s,20,f[1]),Filter(s,21,f[2]));
    default: {
        unsigned i = line-GPS_DIAG_BASE_LINES;
        if (i >= s->top_count) return 0;
        const GPS_DiagSatellite_t *v = &s->top[i];
        return snprintf(out, cap, "GPS SV %s:%u CNO=%u q=%u used=%u health=%u orbit=%u eph=%u alm=%u\r\n",
            v->gnss < 7U ? names[v->gnss] : "OTHER",v->sv,v->cno,v->quality,v->used,
            v->health,v->orbit,v->ephemeris,v->almanac);
    }
    }
}
