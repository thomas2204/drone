#ifndef CAMERA_MP4_H
#define CAMERA_MP4_H
#include "camera_avi.h"
typedef struct {
    CameraAVIWrite write;
    CameraAVISeek seek;
    void *context;
    uint32_t position, frames, mdat_offset;
    uint32_t offsets[CAMERA_AVI_MAX_FRAMES], sizes[CAMERA_AVI_MAX_FRAMES], hashes[CAMERA_AVI_MAX_FRAMES];
    uint8_t sps[256], pps[256];
    uint32_t sps_bytes, pps_bytes;
    bool failed;
} CameraMP4;
bool CameraMP4_Begin(CameraMP4 *, CameraAVIWrite, CameraAVISeek, void *);
bool CameraMP4_Frame(CameraMP4 *, const uint8_t *annex_b, uint32_t bytes);
bool CameraMP4_End(CameraMP4 *, uint32_t duration_ms);
#endif
