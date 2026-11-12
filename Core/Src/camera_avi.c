#include "camera_avi.h"
#include <string.h>

static void U32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}

static bool Put(CameraAVI *a, const void *data, uint32_t bytes)
{
    if (a->failed || !a->write(a->context, data, bytes)) {
        a->failed = true;
        return false;
    }
    a->position += bytes;
    return true;
}

static bool Header(CameraAVI *a, uint32_t file_bytes, uint32_t movi_bytes, uint32_t ms)
{
    uint8_t h[224] = {0};
    memcpy(h, "RIFF", 4); U32(h + 4, file_bytes - 8); memcpy(h + 8, "AVI ", 4);
    memcpy(h + 12, "LIST", 4); U32(h + 16, 192); memcpy(h + 20, "hdrlavih", 8);
    U32(h + 28, 56);
    U32(h + 32, a->frames ? (uint32_t)((uint64_t)ms * 1000U / a->frames) : 250000U);
    U32(h + 36, a->frames ? (uint32_t)((uint64_t)(a->largest + 8U) * a->frames * 1000U / ms) : 0);
    U32(h + 44, a->frames ? 0x10U : 0); /* AVIF_HASINDEX only after finalization */
    U32(h + 48, a->frames); U32(h + 56, 1); U32(h + 60, a->largest);
    U32(h + 64, a->width); U32(h + 68, a->height);
    memcpy(h + 88, "LIST", 4); U32(h + 92, 116); memcpy(h + 96, "strlstrh", 8);
    U32(h + 104, 56); memcpy(h + 108, a->uncompressed ? "vidsDIB " : "vidsMJPG", 8);
    U32(h + 128, a->frames ? ms : 1); U32(h + 132, a->frames ? a->frames * 1000U : 4);
    U32(h + 140, a->frames); U32(h + 144, a->largest); U32(h + 148, 0xFFFFFFFFU);
    h[160] = (uint8_t)a->width; h[161] = (uint8_t)(a->width >> 8);
    h[162] = (uint8_t)a->height; h[163] = (uint8_t)(a->height >> 8);
    memcpy(h + 164, "strf", 4); U32(h + 168, 40); U32(h + 172, 40);
    U32(h + 176, a->width); U32(h + 180, a->height); h[184] = 1; h[186] = 24;
    if (!a->uncompressed) memcpy(h + 188, "MJPG", 4);
    U32(h + 192, ((a->width * 3U + 3U) & ~3U) * a->height);
    memcpy(h + 212, "LIST", 4); U32(h + 216, movi_bytes); memcpy(h + 220, "movi", 4);
    return Put(a, h, sizeof(h));
}

static bool Begin(CameraAVI *a, CameraAVIWrite write, CameraAVISeek seek,
                  void *context, uint16_t width, uint16_t height, bool uncompressed)
{
    if (!a || !write || !seek || !width || !height || width > 32767U || height > 32767U) return false;
    memset(a, 0, sizeof(*a));
    a->write = write; a->seek = seek; a->context = context;
    a->width = width; a->height = height;
    a->uncompressed = uncompressed;
    return Header(a, 224, 4, 1);
}

bool CameraAVI_Begin(CameraAVI *a, CameraAVIWrite write, CameraAVISeek seek,
                     void *context, uint16_t width, uint16_t height)
{
    return Begin(a, write, seek, context, width, height, false);
}

bool CameraAVI_BeginBGR(CameraAVI *a, CameraAVIWrite write, CameraAVISeek seek,
                        void *context, uint16_t width, uint16_t height)
{
    return Begin(a, write, seek, context, width, height, true);
}

bool CameraAVI_Frame(CameraAVI *a, const void *jpeg, uint32_t bytes)
{
    uint32_t raw_bytes = ((a->width * 3U + 3U) & ~3U) * a->height;
    if (a->failed || a->frames >= CAMERA_AVI_MAX_FRAMES || !jpeg || bytes < 4 ||
        (a->uncompressed ? bytes != raw_bytes : bytes > 131072U)) return false;
    uint8_t h[8]; memcpy(h, a->uncompressed ? "00db" : "00dc", 4); U32(h + 4, bytes);
    uint32_t offset = a->position - 220U;
    if (!Put(a, h, sizeof(h)) || !Put(a, jpeg, bytes)) return false;
    const uint8_t pad = 0;
    if ((bytes & 1U) && !Put(a, &pad, 1)) return false;
    a->offsets[a->frames] = offset; a->sizes[a->frames] = bytes; ++a->frames;
    if (bytes > a->largest) a->largest = bytes;
    return true;
}

bool CameraAVI_End(CameraAVI *a, uint32_t duration_ms)
{
    if (a->failed || !a->frames || !duration_ms) return false;
    uint32_t movi_bytes = a->position - 220U;
    uint8_t entry[16] = {0}; memcpy(entry, "idx1", 4); U32(entry + 4, 16U * a->frames);
    if (!Put(a, entry, 8)) return false;
    for (uint32_t i = 0; i < a->frames; ++i) {
        memcpy(entry, a->uncompressed ? "00db" : "00dc", 4); U32(entry + 4, 0x10);
        U32(entry + 8, a->offsets[i]); U32(entry + 12, a->sizes[i]);
        if (!Put(a, entry, sizeof(entry))) return false;
    }
    uint32_t file_bytes = a->position;
    if (!a->seek(a->context, 0)) { a->failed = true; return false; }
    a->position = 0;
    if (!Header(a, file_bytes, movi_bytes, duration_ms) || !a->seek(a->context, file_bytes)) {
        a->failed = true; return false;
    }
    a->position = file_bytes;
    return true;
}
