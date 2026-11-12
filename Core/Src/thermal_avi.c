/* thermal_avi.c
 * Renders MLX90640 frames (bilinear 10x, iron-bow palette) and stores them
 * as an uncompressed 24-bit AVI on the TF card via FatFS.
 *
 * File layout (all little-endian):
 *   0   RIFF  size  'AVI '
 *   12  LIST  192   'hdrl'
 *   24    'avih' 56 ...
 *   88    LIST 116 'strl'
 *   100     'strh' 56 ...
 *   164     'strf' 40 (BITMAPINFOHEADER)
 *   212 LIST  size  'movi'
 *   224   '00db' N <pixels> ...        (one chunk per frame)
 *         'idx1' 16*frames ...
 */
#include "thermal_avi.h"
#include "main.h"
#include "ff.h"
#include <string.h>
#include "thermal_palette.h"
#include "app_storage.h"

#define SRC_W        32
#define SRC_H        24
#define FRAME_BYTES  (AVI_W * AVI_H * 3)       /* 230400, a multiple of 4 */
#define CHUNK_BYTES  (8 + FRAME_BYTES)
#define HDR_SIZE     224
#define MIN_SPAN_C   5.0f                      /* never stretch less than this */
#define RANGE_SMOOTH 0.2f                      /* 0 = frozen, 1 = no smoothing */

/* Static, 32-byte aligned buffers: they must live in AXI SRAM for DMA. */
static uint8_t row[AVI_W * 3];
static uint8_t hdr[HDR_SIZE];
static uint8_t idxbuf[512];
static uint8_t io[1024] __attribute__((section(".sd_dma_buffer"), aligned(32)));
static FIL vf __attribute__((section(".sd_dma_buffer"), aligned(32)));

/* Bounce from DTCM/stack to AXI SRAM. Split at the partial-sector boundary
 * so FatFs never sends an unaligned interior pointer to SDMMC IDMA. */
static int write_all(const void *source, UINT bytes)
{
    const uint8_t *p = source;
    while (bytes) {
        UINT partial = (UINT)(f_tell(&vf) & 511U);
        UINT count = partial ? 512U - partial : sizeof(io);
        if (count > bytes) count = bytes;
        memcpy(io, p, count);
        UINT written = 0;
        if (f_write(&vf, io, count, &written) != FR_OK || written != count) return -1;
        p += count;
        bytes -= count;
    }
    return 0;
}

static uint8_t  lut[256][3];                   /* B, G, R */
static uint16_t xi[AVI_W], yi[AVI_H];
static float    xf[AVI_W], yf[AVI_H];
static int      tables_ready;

static uint32_t n_frames, t_start_ms, t_last_ms;
static float    smin, smax;
static int      range_ready;
static int      is_open;

/* ---- little-endian helpers ---- */
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void pf(uint8_t *p, const char *s) { memcpy(p, s, 4); }

/* ---- lookup tables ---- */
static void tables_init(void)
{
    //for (int i = 0; i < 256; i++) {            /* cold = blue, hot = red */
      //  lut[i][0] = (uint8_t)(255 - i);        /* B */
        //lut[i][1] = 0;                         /* G */
        //lut[i][2] = (uint8_t)i;                /* R */
    //}
	palette_init(lut,p_iron_bow);


    for (int x = 0; x < AVI_W; x++) {
        float sx = (float)x * (SRC_W - 1) / (AVI_W - 1);
        int i = (int)sx;
        if (i > SRC_W - 2) i = SRC_W - 2;
        xi[x] = (uint16_t)i;
        xf[x] = sx - (float)i;
    }
    for (int y = 0; y < AVI_H; y++) {
        float sy = (float)y * (SRC_H - 1) / (AVI_H - 1);
        int i = (int)sy;
        if (i > SRC_H - 2) i = SRC_H - 2;
        yi[y] = (uint16_t)i;
        yf[y] = sy - (float)i;
    }
    tables_ready = 1;
}

/* ---- AVI header ---- */
static void build_header(uint32_t frames, uint32_t elapsed_ms)
{
    uint32_t file_size = HDR_SIZE + frames * CHUNK_BYTES + 8 + frames * 16;
    uint32_t usec = 250000;                    /* 4 fps placeholder until closed */
    uint32_t rate = 4000;                      /* fps x 1000 */

    if (frames > 0 && elapsed_ms > 0) {
        usec = (uint32_t)(((uint64_t)elapsed_ms * 1000u) / frames);
        rate = (uint32_t)(((uint64_t)frames * 1000000u) / elapsed_ms);
        if (rate == 0) rate = 1;
    }
    memset(hdr, 0, HDR_SIZE);

    pf(hdr + 0, "RIFF");  p32(hdr + 4, file_size - 8);  pf(hdr + 8, "AVI ");
    pf(hdr + 12, "LIST"); p32(hdr + 16, 192);           pf(hdr + 20, "hdrl");

    pf(hdr + 24, "avih"); p32(hdr + 28, 56);
    p32(hdr + 32, usec);                        /* dwMicroSecPerFrame */
    p32(hdr + 36, (uint32_t)(((uint64_t)FRAME_BYTES * rate) / 1000u)); /* bytes/s */
    p32(hdr + 44, 0x10);                        /* AVIF_HASINDEX */
    p32(hdr + 48, frames);                      /* dwTotalFrames */
    p32(hdr + 56, 1);                           /* dwStreams */
    p32(hdr + 60, FRAME_BYTES);                 /* dwSuggestedBufferSize */
    p32(hdr + 64, AVI_W);
    p32(hdr + 68, AVI_H);

    pf(hdr + 88, "LIST"); p32(hdr + 92, 116);   pf(hdr + 96, "strl");

    pf(hdr + 100, "strh"); p32(hdr + 104, 56);
    pf(hdr + 108, "vids");                      /* fccType */
    pf(hdr + 112, "DIB ");                      /* fccHandler: uncompressed */
    p32(hdr + 128, 1000);                       /* dwScale */
    p32(hdr + 132, rate);                       /* dwRate  (rate/scale = fps) */
    p32(hdr + 140, frames);                     /* dwLength */
    p32(hdr + 144, FRAME_BYTES);                /* dwSuggestedBufferSize */
    p32(hdr + 148, 0xFFFFFFFFu);                /* dwQuality: default */
    p16(hdr + 160, AVI_W);                      /* rcFrame right (left/top stay 0) */
    p16(hdr + 162, AVI_H);                      /* rcFrame bottom */

    pf(hdr + 164, "strf"); p32(hdr + 168, 40);
    p32(hdr + 172, 40);                         /* biSize */
    p32(hdr + 176, AVI_W);                      /* biWidth */
    p32(hdr + 180, AVI_H);                      /* biHeight (positive = bottom-up) */
    p16(hdr + 184, 1);                          /* biPlanes */
    p16(hdr + 186, 24);                         /* biBitCount */
    p32(hdr + 188, 0);                          /* biCompression = BI_RGB */
    p32(hdr + 192, FRAME_BYTES);                /* biSizeImage */

    pf(hdr + 212, "LIST"); p32(hdr + 216, 4 + frames * CHUNK_BYTES); pf(hdr + 220, "movi");
}

/* ---- rendering ---- */
static int render(const float *To)
{
    float mn = To[0], mx = To[0];
    for (int i = 1; i < SRC_W * SRC_H; i++) {
        if (To[i] < mn) mn = To[i];
        if (To[i] > mx) mx = To[i];
    }
    if (!range_ready) {
        smin = mn; smax = mx; range_ready = 1;
    } else {                                   /* smooth the scale: no flicker */
        smin += (mn - smin) * RANGE_SMOOTH;
        smax += (mx - smax) * RANGE_SMOOTH;
    }
    float lo = smin, hi = smax;
    if (hi - lo < MIN_SPAN_C) {
        float mid = 0.5f * (hi + lo);
        lo = mid - 0.5f * MIN_SPAN_C;
        hi = mid + 0.5f * MIN_SPAN_C;
    }
    float k = 255.0f / (hi - lo);

    for (int y = AVI_H - 1; y >= 0; y--) {
        const float *r0 = To + yi[y] * SRC_W;
        const float *r1 = r0 + SRC_W;
        float fy = yf[y];
        /* AVI rows are stored bottom-up */
        uint8_t *d = row;
        for (int x = 0; x < AVI_W; x++) {
            int i = xi[x];
            float fx = xf[x];
            float top = r0[i] + (r0[i + 1] - r0[i]) * fx;
            float bot = r1[i] + (r1[i + 1] - r1[i]) * fx;
            float t = top + (bot - top) * fy;
            int idx = (int)((t - lo) * k);
            if (idx < 0)   idx = 0;
            if (idx > 255) idx = 255;
            d[0] = lut[idx][0];
            d[1] = lut[idx][1];
            d[2] = lut[idx][2];
            d += 3;
        }
        if (write_all(row, sizeof(row))) return -1;
    }
    return 0;
}

/* ---- public API ---- */
int avi_open(const char *name)
{
    if (!tables_ready) tables_init();
    if (AppStorage_Open(&vf, name, FA_CREATE_NEW | FA_WRITE) != FR_OK) return -1;

    n_frames = 0;
    range_ready = 0;
    t_start_ms = HAL_GetTick();
    t_last_ms = t_start_ms;
    build_header(0, 0);
    if (write_all(hdr, HDR_SIZE)) {
        AppStorage_Close(&vf);
        return -2;
    }
    is_open = 1;
    return 0;
}

int avi_add_frame(const float *To)
{
    if (!is_open) return -1;
    uint32_t frame_start = (uint32_t)f_tell(&vf);
    uint8_t chunk_header[8];
    pf(chunk_header, "00db");
    p32(chunk_header + 4, FRAME_BYTES);
    if (write_all(chunk_header, sizeof(chunk_header)) || render(To)) {
        /* Discard a partially written frame before appending the index. */
        if (f_lseek(&vf, frame_start) != FR_OK || f_truncate(&vf) != FR_OK) {
            AppStorage_Close(&vf);
            is_open = 0;
        }
        return -2;
    }
    n_frames++;
    t_last_ms = HAL_GetTick();
    if ((n_frames & 7) == 0 && f_sync(&vf) != FR_OK) return -3;
    return 0;
}

int avi_close_duration(uint32_t duration_ms)
{
    uint32_t n = 0;

    if (!is_open) return -1;
    is_open = 0;

    /* idx1: one 16-byte entry per frame, offsets relative to the 'movi' tag */
    pf(idxbuf, "idx1");
    p32(idxbuf + 4, n_frames * 16);
    if (write_all(idxbuf, 8)) { AppStorage_Close(&vf); return -2; }

    for (uint32_t i = 0; i < n_frames; i++) {
        uint8_t *e = idxbuf + n;
        pf(e, "00db");
        p32(e + 4, 0x10);                           /* AVIIF_KEYFRAME */
        p32(e + 8, 4 + i * CHUNK_BYTES);            /* offset from 'movi' */
        p32(e + 12, FRAME_BYTES);
        n += 16;
        if (n == sizeof(idxbuf) || i == n_frames - 1) {
            if (write_all(idxbuf, n)) { AppStorage_Close(&vf); return -3; }
            n = 0;
        }
    }

    /* rewrite the header now that the real frame count and speed are known */
    build_header(n_frames, duration_ms ? duration_ms : t_last_ms - t_start_ms);
    if (f_lseek(&vf, 0) != FR_OK) { AppStorage_Close(&vf); return -4; }
    if (write_all(hdr, HDR_SIZE)) { AppStorage_Close(&vf); return -5; }
    return (AppStorage_Close(&vf) == FR_OK) ? 0 : -6;
}

uint32_t avi_frames(void)
{
    return n_frames;
}

int avi_close(void)
{
    return avi_close_duration(0);
}
