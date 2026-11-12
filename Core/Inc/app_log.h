#ifndef APP_LOG_H
#define APP_LOG_H
#include "main.h"
#include <stdbool.h>
bool AppLog_Init(void);
HAL_StatusTypeDef AppLog_Write(const void *, uint16_t, uint32_t);
#endif
