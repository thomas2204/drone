#include "ld6002.h"
#include <string.h>
#include "cmsis_os2.h"
#include "usart.h"
#include "ff.h"
#include "app_storage.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

typedef enum { S_SOF, S_HDR, S_HCK, S_DATA, S_DCK } st_t;

static ld6002_data_t d;
static st_t   st;
static uint8_t hdr[7];       /* SOF ID(2) LEN(2) TYPE(2) */
static uint8_t hn, hx;
static uint8_t pay[LD_MAX_PAYLOAD];
static uint16_t len, type, pn;
static uint8_t px, skip;


void ld6002_init(void){ memset(&d,0,sizeof d); st=S_SOF; }
const ld6002_data_t *ld6002_get(void){ return &d; }

static float f32(const uint8_t *p){ float f; memcpy(&f,p,4); return f; }   /* little-endian MCU */
static uint32_t u32(const uint8_t *p){ uint32_t v; memcpy(&v,p,4); return v; }

static void decode(uint32_t now)
{
    switch (type) {
    case LD_TYPE_PRESENCE: if(len>=1){ d.present = pay[0]; d.last_update_ms[LD_UPD_PRES]=now; } break;
    case LD_TYPE_BREATH:   if(len>=4){ d.breath_bpm=f32(pay); d.last_update_ms[LD_UPD_BREATH]=now; } break;
    case LD_TYPE_HEART:    if(len>=4){ d.heart_bpm =f32(pay); d.last_update_ms[LD_UPD_HEART]=now; } break;
    case LD_TYPE_PHASE:    if(len>=12){ d.phase_total=f32(pay); d.phase_breath=f32(pay+4); d.phase_heart=f32(pay+8); d.last_update_ms[LD_UPD_PHASE]=now; } break;
    case LD_TYPE_RANGE:    if(len>=8){ d.range_valid=(u32(pay)==1); d.range_cm=f32(pay+4); d.last_update_ms[LD_UPD_RANGE]=now; } break;
    case LD_TYPE_TRACK_POS:if(len>=12){ d.x=f32(pay); d.y=f32(pay+4); d.z=f32(pay+8); d.last_update_ms[LD_UPD_POS]=now; } break;
    default: break;
    }
}

int ld6002_feed(uint8_t b, uint32_t now)
{
    switch (st) {
    case S_SOF:
        if (b == 0x01) { hdr[0]=b; hx=b; hn=1; st=S_HDR; }
        break;
    case S_HDR:
        hdr[hn++]=b; hx^=b;
        if (hn==7) { st=S_HCK; }
        break;
    case S_HCK:
        if ((uint8_t)~hx != b) { d.frames_bad++; st=S_SOF; break; }
        len  = (hdr[3]<<8)|hdr[4];
        type = (hdr[5]<<8)|hdr[6];
        pn=0; px=0; skip = (len > LD_MAX_PAYLOAD);
        if (len==0) { st=S_SOF; d.frames_ok++; return 1; }   /* no DATA_CKSUM when LEN==0 */
        st=S_DATA;
        break;
    case S_DATA:
        if (!skip) pay[pn]=b;
        pn++; px^=b;
        if (pn==len) st=S_DCK;
        break;
    case S_DCK:
        st=S_SOF;
        if ((uint8_t)~px != b) { d.frames_bad++; break; }
        d.frames_ok++;
        if (!skip) decode(now);
        return 1;
    }
    return 0;
}

