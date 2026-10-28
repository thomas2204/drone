#include "gps_ubx.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint16_t U16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t U32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t I32(const uint8_t *p)
{
    uint32_t value = U32(p);
    return value <= INT32_MAX ? (int32_t)value :
        (int32_t)((int64_t)value - 4294967296LL);
}

bool GPS_UbxDecodePvt(const uint8_t *p, size_t size, GPS_UbxPvt_t *out)
{
    if (p == NULL || out == NULL || size != GPS_UBX_PVT_SIZE) return false;
    GPS_UbxPvt_t next = {0};
    next.tow_ms = U32(p);
    next.year = U16(p + 4);
    next.month = p[6];
    next.day = p[7];
    next.hour = p[8];
    next.minute = p[9];
    next.second = p[10];
    next.valid = p[11];
    next.nano = I32(p + 16);
    next.fix_type = p[20];
    next.flags = p[21];
    next.satellites = p[23];
    next.longitude_e7 = I32(p + 24);
    next.latitude_e7 = I32(p + 28);
    next.altitude_msl_mm = I32(p + 36);
    next.horizontal_accuracy_mm = U32(p + 40);
    next.vertical_accuracy_mm = U32(p + 44);
    next.velocity_n_mm_s = I32(p + 48);
    next.velocity_e_mm_s = I32(p + 52);
    next.velocity_d_mm_s = I32(p + 56);
    next.ground_speed_mm_s = I32(p + 60);
    next.speed_accuracy_mm_s = U32(p + 68);
    // gnssFixOK plus a GNSS fix (not dead reckoning or time-only).
    // flags3.invalidLlh invalidates the position/height fields.
    next.fix_ok = (next.flags & 1U) != 0U && (U16(p + 78) & 1U) == 0U &&
        next.fix_type >= 2U && next.fix_type <= 4U;
    if (next.tow_ms >= GPS_UBX_WEEK_MS || next.fix_type > 5U ||
        next.nano < -1000000000 || next.nano > 1000000000) return false;
    if (next.fix_ok &&
        (next.latitude_e7 < -900000000 || next.latitude_e7 > 900000000 ||
         next.longitude_e7 < -1800000000 || next.longitude_e7 > 1800000000 ||
         next.velocity_n_mm_s < -500000 || next.velocity_n_mm_s > 500000 ||
         next.velocity_e_mm_s < -500000 || next.velocity_e_mm_s > 500000 ||
         next.velocity_d_mm_s < -500000 || next.velocity_d_mm_s > 500000)) return false;
    *out = next;
    return true;
}

void GPS_UbxReset(GPS_UbxParser_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool GPS_UbxBusy(const GPS_UbxParser_t *p)
{
    return p->state != 0U;
}

static void Checksum(GPS_UbxParser_t *p, uint8_t byte)
{
    p->ck_a = (uint8_t)(p->ck_a + byte);
    p->ck_b = (uint8_t)(p->ck_b + p->ck_a);
}

GPS_UbxEvent_t GPS_UbxFeed(GPS_UbxParser_t *p, uint8_t byte)
{
    switch (p->state)
    {
        case 0:
            if (byte == 0xB5U) p->state = 1;
            break;
        case 1:
            if (byte == 0x62U)
            {
                p->state = 2;
                p->ck_a = p->ck_b = 0;
            }
            else if (byte != 0xB5U) p->state = 0;
            break;
        case 2:
            p->message_class = byte;
            Checksum(p, byte);
            p->state = 3;
            break;
        case 3:
            p->message_id = byte;
            Checksum(p, byte);
            p->state = 4;
            break;
        case 4:
            p->length = byte;
            Checksum(p, byte);
            p->state = 5;
            break;
        case 5:
            p->length |= (uint16_t)byte << 8;
            Checksum(p, byte);
            p->position = 0;
            // Bound recovery time after a corrupt length. Unknown large frames
            // are discarded; NAV-PVT (92), NAV-DOP (18) and ACK (2) are decoded.
            if (p->length > 1024U)
            {
                p->state = 0;
                return GPS_UBX_BAD_FRAME;
            }
            p->state = p->length != 0U ? 6 : 7;
            break;
        case 6:
            if (p->position < sizeof(p->payload)) p->payload[p->position] = byte;
            p->position++;
            Checksum(p, byte);
            if (p->position == p->length) p->state = 7;
            break;
        case 7:
            p->received_ck_a = byte;
            p->state = 8;
            break;
        case 8:
            p->state = 0;
            if (p->received_ck_a != p->ck_a || byte != p->ck_b)
            {
                if (p->received_ck_a == 0xB5U && byte == 0x62U)
                {
                    p->state = 2;
                    p->ck_a = p->ck_b = 0;
                }
                else if (byte == 0xB5U) p->state = 1;
                return GPS_UBX_BAD_FRAME;
            }
            if (p->message_class == 1U && p->message_id == 7U)
                return GPS_UbxDecodePvt(p->payload, p->length, &p->pvt)
                    ? GPS_UBX_PVT : GPS_UBX_BAD_PVT;
            if (p->message_class == 1U && p->message_id == 4U)
            {
                if (p->length != 18U || U32(p->payload) >= GPS_UBX_WEEK_MS)
                    return GPS_UBX_BAD_DOP;
                p->dop.tow_ms = U32(p->payload);
                p->dop.vdop_x100 = U16(p->payload + 10);
                p->dop.hdop_x100 = U16(p->payload + 12);
                return GPS_UBX_DOP;
            }
            if (p->message_class == 5U && p->length == 2U && p->message_id <= 1U)
            {
                p->acknowledged_class = p->payload[0];
                p->acknowledged_id = p->payload[1];
                return p->message_id == 1U ? GPS_UBX_ACK : GPS_UBX_NAK;
            }
            break;
        default:
            GPS_UbxReset(p);
            break;
    }
    return GPS_UBX_NONE;
}

size_t GPS_UbxBuild10HzConfig(uint8_t *out, size_t capacity)
{
    // u-blox M10 SPG 5.10 interface: CFG-RATE, CFG-MSGOUT,
    // CFG-UART1OUTPROT and CFG-VALSET version 0 (RAM layer only).
    static const uint8_t packet[] = {
        0xB5,0x62,0x06,0x8A,0x24,0x00,
        0x00,0x01,0x00,0x00,
        0x01,0x00,0x21,0x30,0x64,0x00, // CFG-RATE-MEAS: 100 ms.
        0x02,0x00,0x21,0x30,0x01,0x00, // CFG-RATE-NAV: every measurement.
        0x07,0x00,0x91,0x20,0x01,      // NAV-PVT UART1: every solution.
        0x39,0x00,0x91,0x20,0x01,      // NAV-DOP UART1: same epoch.
        0x01,0x00,0x74,0x10,0x01,      // UART1 output: UBX on.
        0x02,0x00,0x74,0x10,0x00,      // UART1 output: NMEA off.
        0x00,0x00
    };
    if (out == NULL || capacity < sizeof(packet)) return 0;
    memcpy(out, packet, sizeof(packet));
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < sizeof(packet) - 2U; ++i)
    {
        a = (uint8_t)(a + out[i]);
        b = (uint8_t)(b + a);
    }
    out[sizeof(packet) - 2U] = a;
    out[sizeof(packet) - 1U] = b;
    return sizeof(packet);
}

int GPS_UbxFormatPvt(char *out, size_t capacity, const GPS_UbxPvt_t *p)
{
    if (!p->fix_ok)
        return snprintf(out, capacity, "GPS UBX NO FIX Type=%u Sat=%u\r\n",
                        (unsigned)p->fix_type, (unsigned)p->satellites);
    int64_t lat = p->latitude_e7, lon = p->longitude_e7, alt = p->altitude_msl_mm;
    if (lat < 0) lat = -lat;
    if (lon < 0) lon = -lon;
    if (alt < 0) alt = -alt;
    return snprintf(out, capacity,
        "GPS UBX Lat=%s%ld.%07lu Lon=%s%ld.%07lu AltMSL=%s%ld.%03lu m Sat=%u Fix=%u\r\n",
        p->latitude_e7 < 0 ? "-" : "", (long)(lat / 10000000),
        (unsigned long)(lat % 10000000), p->longitude_e7 < 0 ? "-" : "",
        (long)(lon / 10000000), (unsigned long)(lon % 10000000),
        p->altitude_msl_mm < 0 ? "-" : "", (long)(alt / 1000),
        (unsigned long)(alt % 1000), (unsigned)p->satellites, (unsigned)p->fix_type);
}
