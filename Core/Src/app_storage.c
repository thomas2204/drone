#include "app_storage.h"
#include "cmsis_os2.h"
#include "sdmmc.h"

static osMutexId_t volume_mutex;
volatile uint32_t app_storage_open_files;
volatile bool app_storage_mounted;

bool AppStorage_Init(void)
{
    const osMutexAttr_t attr = {.name="sd_volume", .attr_bits=osMutexPrioInherit};
    if (!volume_mutex) volume_mutex=osMutexNew(&attr);
    return volume_mutex != NULL;
}

static FRESULT MountLocked(void)
{
    if (app_storage_mounted) return FR_OK;
    if (retSD != 0U) return FR_NOT_ENABLED;
    /* HAL can return to READY after a failed identification command. A card
     * geometry is also required before reusing an initialized controller. */
    if (HAL_SD_GetState(&hsd1) != HAL_SD_STATE_READY || hsd1.SdCard.BlockNbr == 0U) {
        hsd1.Instance=SDMMC1;
        hsd1.Init.ClockEdge=SDMMC_CLOCK_EDGE_RISING;
        hsd1.Init.ClockPowerSave=SDMMC_CLOCK_POWER_SAVE_DISABLE;
        hsd1.Init.BusWide=SDMMC_BUS_WIDE_1B;
        hsd1.Init.HardwareFlowControl=SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
        hsd1.Init.ClockDiv=4;
        if (HAL_SD_Init(&hsd1) != HAL_OK ||
            HAL_SD_ConfigWideBusOperation(&hsd1,SDMMC_BUS_WIDE_4B) != HAL_OK) return FR_NOT_READY;
    }
    FRESULT result=f_mount(&SDFatFS,SDPath,1);
    app_storage_mounted=result==FR_OK;
    return result;
}

FRESULT AppStorage_Mount(void)
{
    if (!volume_mutex || osMutexAcquire(volume_mutex,osWaitForever)!=osOK) return FR_TIMEOUT;
    FRESULT result=MountLocked();
    osMutexRelease(volume_mutex);return result;
}

FRESULT AppStorage_Open(FIL *file,const TCHAR *path,BYTE mode)
{
    if (!volume_mutex || osMutexAcquire(volume_mutex,osWaitForever)!=osOK) return FR_TIMEOUT;
    FRESULT result=MountLocked();
    if (result==FR_OK) {
        result=f_open(file,path,mode);
        if (result==FR_OK) ++app_storage_open_files;
    }
    osMutexRelease(volume_mutex);return result;
}

FRESULT AppStorage_Close(FIL *file)
{
    if (!volume_mutex || osMutexAcquire(volume_mutex,osWaitForever)!=osOK) return FR_TIMEOUT;
    FRESULT result=f_close(file);
    if (result==FR_OK && app_storage_open_files) --app_storage_open_files;
    osMutexRelease(volume_mutex);return result;
}

FRESULT AppStorage_UnmountIfIdle(void)
{
    if (!volume_mutex || osMutexAcquire(volume_mutex,osWaitForever)!=osOK) return FR_TIMEOUT;
    FRESULT result=FR_OK;
    if (app_storage_mounted && app_storage_open_files==0U) {
        result=f_mount(NULL,SDPath,0);
        if (result==FR_OK) app_storage_mounted=false;
    }
    osMutexRelease(volume_mutex);return result;
}
