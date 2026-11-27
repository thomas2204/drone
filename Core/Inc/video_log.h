#ifndef VIDEO_LOG_H
#define VIDEO_LOG_H
#include <stdbool.h>
#include <stdint.h>
#include "ff.h"
typedef struct {
    uint32_t rows;
    FRESULT result;
    char filename[20];
    uint32_t bytes, verified_bytes;
} VideoLogReport;
extern volatile VideoLogReport video_log;
void VideoLog_Begin(void);
void VideoLog_Sample(uint32_t frame);
bool VideoLog_Name(char out[20], const char *video);
FRESULT VideoLog_Write(FIL *file, const char *video, uint32_t frames,
                      uint32_t duration_ms, uint32_t start_ms,
                      uint8_t *scratch, uint32_t capacity);
#endif
