#include "camera_sdram.h"
#include "fmc.h"

#define SDRAM_BASE 0xC0000000UL
#define SDRAM_BYTES (2UL * 1024UL * 1024UL)
volatile CameraSDRAMReport camera_sdram;

static bool Command(uint32_t mode, uint32_t refreshes, uint32_t value)
{
    FMC_SDRAM_CommandTypeDef command = {
        .CommandMode = mode, .CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1,
        .AutoRefreshNumber = refreshes, .ModeRegisterDefinition = value
    };
    camera_sdram.hal_status = HAL_SDRAM_SendCommand(&hsdram1, &command, 100);
    return camera_sdram.hal_status == HAL_OK;
}

bool CameraSDRAM_Init(void)
{
    camera_sdram.result = -1;
    /* Reuse the bench-validated 200 MHz FMC / 2 clock and geometry.
     * PG1 stays AF12. Never expose the aliased upper part of the chip. */
    if (__HAL_RCC_GET_FMC_SOURCE() != RCC_FMCCLKSOURCE_D1HCLK ||
        HAL_RCC_GetHCLKFreq() != 200000000U ||
        (SCB->CCR & SCB_CCR_DC_Msk)) return false;
    if (!Command(FMC_SDRAM_CMD_CLK_ENABLE, 1, 0)) return false;
    HAL_Delay(2);
    if (!Command(FMC_SDRAM_CMD_PALL, 1, 0) ||
        !Command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8, 0) ||
        !Command(FMC_SDRAM_CMD_LOAD_MODE, 1, 0x220U)) return false;
    camera_sdram.hal_status = HAL_SDRAM_ProgramRefreshRate(&hsdram1, 761);
    if (camera_sdram.hal_status != HAL_OK) return false;

    MPU_Region_InitTypeDef region = {0};
    HAL_MPU_Disable();
    region.Enable = MPU_REGION_ENABLE;
    region.Number = MPU_REGION_NUMBER1;
    region.BaseAddress = SDRAM_BASE;
    region.Size = MPU_REGION_SIZE_2MB;
    region.AccessPermission = MPU_REGION_FULL_ACCESS;
    region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    region.TypeExtField = MPU_TEX_LEVEL1; /* Normal, noncacheable. */
    region.IsShareable = MPU_ACCESS_SHAREABLE;
    region.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    region.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&region);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
    __DSB();
    __ISB();

    /* Destructive boot check before any camera/DMA users are started.
     * Index patterns detect address aliasing that constant fills miss. */
    volatile uint32_t *memory = (volatile uint32_t *)SDRAM_BASE;
    for (unsigned pass = 0; pass < 6; ++pass) {
        for (uint32_t i = 0; i < SDRAM_BYTES / 4; ++i) {
            uint32_t value = pass == 0 ? 0U : pass == 1 ? UINT32_MAX :
                pass == 2 ? 0xAAAAAAAAU : pass == 3 ? 0x55555555U :
                pass == 4 ? i : ~i;
            memory[i] = value;
        }
        __DSB();
        HAL_Delay(100);
        for (uint32_t i = 0; i < SDRAM_BYTES / 4; ++i) {
            uint32_t expected = pass == 0 ? 0U : pass == 1 ? UINT32_MAX :
                pass == 2 ? 0xAAAAAAAAU : pass == 3 ? 0x55555555U :
                pass == 4 ? i : ~i;
            uint32_t actual = memory[i];
            if (actual != expected) {
                camera_sdram.address = SDRAM_BASE + i * 4;
                camera_sdram.expected = expected;
                camera_sdram.actual = actual;
                camera_sdram.result = -2;
                return false;
            }
        }
    }
    camera_sdram.tested_bytes = SDRAM_BYTES;
    camera_sdram.result = 1;
    return true;
}
