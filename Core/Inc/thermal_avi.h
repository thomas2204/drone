#ifndef THERMAL_AVI_H
#define THERMAL_AVI_H

#include <stdint.h>

/* Output video: 320x240, 24-bit uncompressed AVI. Player support varies.
 * Each 32x24 thermal frame is bilinear-interpolated 10x and colored with
 * the imported iron-bow palette before it is written. */
#define AVI_W 320
#define AVI_H 240

/* Create the file and write a placeholder header. 0 = OK. */
int      avi_open(const char *name);

/* Render one temperature frame (To: 768 floats, 24 rows x 32 columns, deg C)
 * and append it. 0 = OK. */
int      avi_add_frame(const float *To);

/* Write the index and patch the header with the real frame count and speed.
 * The file is only a valid video after this call. 0 = OK. */
int      avi_close(void);
/* Finalize a command-length clip with the common recording duration. */
int      avi_close_duration(uint32_t duration_ms);

uint32_t avi_frames(void);

#endif
