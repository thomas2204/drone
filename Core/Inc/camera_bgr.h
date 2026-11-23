#ifndef CAMERA_BGR_H
#define CAMERA_BGR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Decode baseline JPEG to a bottom-up, 4-byte-row-aligned Windows RGB24 DIB.
 * Single camera-task decoder; no dynamic allocation or external SDRAM. */
bool CameraBGR_Decode(const uint8_t *, size_t, uint8_t *, uint16_t, uint16_t);
/* Decode a 640x480 JPEG to an 80x60 top-down luminance image. */
bool CameraJPEG_DecodeFlowGray(const uint8_t *jpeg, size_t bytes, uint8_t *pixels);
#endif
