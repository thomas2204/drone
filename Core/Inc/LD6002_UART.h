#ifndef LD6002_UART_H
#define LD6002_UART_H

#include "main.h"
#include "cmsis_os2.h"
#include "usart.h"
#include "ff.h"
#include "app_storage.h"
#include "LD6002.h"

void LD6002_Start(void);
void LD6002_Poll(void);
void LD6002_RxRestart(void);
void log_frame(uint32_t now, const ld6002_data_t *d);
#endif /* LD6002_UART_H */