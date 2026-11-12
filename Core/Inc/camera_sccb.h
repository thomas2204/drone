#ifndef CAMERA_SCCB_H
#define CAMERA_SCCB_H
#include "main.h"
#include <stdbool.h>
/* Diagnostic SCCB master on PB8/PB9. Both outputs are open-drain. */
bool CameraSCCB_Init(void);
HAL_StatusTypeDef CameraSCCB_Register(uint8_t reg, uint8_t *value, bool read);
extern uint32_t camera_sccb_nacks;
#endif
