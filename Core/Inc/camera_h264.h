#ifndef CAMERA_H264_H
#define CAMERA_H264_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool CameraH264_Init(void);
/* BGR24 bottom-up scratch is consumed and becomes the encoded output. */
bool CameraH264_Encode(uint8_t *bgr_scratch, size_t scratch_bytes,
                       const uint8_t **output, uint32_t *bytes);
#endif
