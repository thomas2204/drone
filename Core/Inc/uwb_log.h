#ifndef UWB_LOG_H
#define UWB_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* 1 = start logging at boot; 0 = wait for the Bluetooth command "uwb log". */
#ifndef UWB_LOG_AUTOSTART
#define UWB_LOG_AUTOSTART 1
#endif

/* FreeRTOS task body. Reads UWB_RangeGetSnapshot() (a locked copy), so it never
 * touches SPI5 or the DW3000 driver. Writes UWB.CSV on the SD card. */
void UWB_LogTask(void *argument);

/* Pause/resume logging (the file is closed while paused). */
void UWB_LogSetEnabled(bool enabled);
bool UWB_LogIsEnabled(void);

#endif
