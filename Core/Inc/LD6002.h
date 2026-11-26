#ifndef LD6002_H
#define LD6002_H
#include <stdint.h>

/* Hi-Link LD6002 "TF" frame parser (protocol V1.3).
 * Frame: 01 | ID(2,BE) | LEN(2,BE) | TYPE(2,BE) | HEAD_CKSUM | DATA(LEN, little-endian) | DATA_CKSUM
 * CKSUM = ~(XOR of bytes). DATA_CKSUM is absent when LEN == 0. */

#define LD_TYPE_FW_QUERY   0xFFFF
#define LD_TYPE_PRESENCE   0x0F09
#define LD_TYPE_TARGETS    0x0A04
#define LD_TYPE_PHASE      0x0A13
#define LD_TYPE_BREATH     0x0A14
#define LD_TYPE_HEART      0x0A15
#define LD_TYPE_RANGE      0x0A16
#define LD_TYPE_TRACK_POS  0x0A17

#define LD_MAX_PAYLOAD 128   /* bigger frames (e.g. point cloud 0x0A08) are skipped */

typedef struct {
    int      present;            /* 0x0F09 */
    float    breath_bpm;         /* 0x0A14 */
    float    heart_bpm;          /* 0x0A15 */
    float    phase_total, phase_breath, phase_heart; /* 0x0A13 */
    int      range_valid;        /* 0x0A16 flag */
    float    range_cm;           /* 0x0A16 */
    float    x, y, z;            /* 0x0A17, metres */
    uint32_t last_update_ms[8];  /* index: see LD_UPD_* */
    uint32_t frames_ok, frames_bad;
} ld6002_data_t;

enum { LD_UPD_PRES, LD_UPD_BREATH, LD_UPD_HEART, LD_UPD_PHASE, LD_UPD_RANGE, LD_UPD_POS };

void ld6002_init(void);
/* Feed one received byte; now_ms = HAL_GetTick(). Returns 1 when a frame was decoded. */
int  ld6002_feed(uint8_t b, uint32_t now_ms);
const ld6002_data_t *ld6002_get(void);
void ld6002_log_frame(uint32_t now);
#endif