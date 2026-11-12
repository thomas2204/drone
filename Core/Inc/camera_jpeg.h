#ifndef CAMERA_JPEG_H
#define CAMERA_JPEG_H
#include <stddef.h>
#include <stdint.h>
/* Locate a complete JPEG and check its SOF dimensions. */
int CameraJPEG_Find(const uint8_t *data, size_t length, size_t *offset,
                    size_t *size, uint16_t *width, uint16_t *height);
#endif
