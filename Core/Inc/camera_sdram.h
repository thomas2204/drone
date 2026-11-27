#ifndef CAMERA_SDRAM_H
#define CAMERA_SDRAM_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* 0 pending, 1 ready, -1 initialization failure, -2 memory failure. */
    int32_t result;
    uint32_t hal_status, tested_bytes, address, expected, actual;
} CameraSDRAMReport;
extern volatile CameraSDRAMReport camera_sdram;
/* Call once after MX_FMC_Init, before tasks or buffer access. */
bool CameraSDRAM_Init(void);
#endif
