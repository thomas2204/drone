#ifndef APP_STORAGE_H
#define APP_STORAGE_H
#include "fatfs.h"
#include <stdbool.h>
/* Initialize once before tasks. FatFs already serializes individual file I/O;
 * this lock additionally protects volume lifetime and SD initialization. */
bool AppStorage_Init(void);
FRESULT AppStorage_Mount(void);
FRESULT AppStorage_Open(FIL *, const TCHAR *, BYTE);
FRESULT AppStorage_Close(FIL *);
FRESULT AppStorage_UnmountIfIdle(void);
extern volatile uint32_t app_storage_open_files;
extern volatile bool app_storage_mounted;
#endif
