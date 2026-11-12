#ifndef CAMERA_AVI_H
#define CAMERA_AVI_H
#include <stdbool.h>
#include <stdint.h>
#define CAMERA_AVI_MAX_FRAMES 120U
typedef bool (*CameraAVIWrite)(void *, const void *, uint32_t);
typedef bool (*CameraAVISeek)(void *, uint32_t);
typedef struct {
    CameraAVIWrite write;
    CameraAVISeek seek;
    void *context;
    uint32_t frames, position, largest;
    uint32_t offsets[CAMERA_AVI_MAX_FRAMES], sizes[CAMERA_AVI_MAX_FRAMES];
    uint16_t width, height;
    bool failed, uncompressed;
} CameraAVI;
bool CameraAVI_Begin(CameraAVI *, CameraAVIWrite, CameraAVISeek, void *, uint16_t, uint16_t);
bool CameraAVI_BeginBGR(CameraAVI *, CameraAVIWrite, CameraAVISeek, void *, uint16_t, uint16_t);
bool CameraAVI_Frame(CameraAVI *, const void *, uint32_t);
/* Final playback rate uses measured clip duration; call before closing file. */
bool CameraAVI_End(CameraAVI *, uint32_t duration_ms);
#endif
