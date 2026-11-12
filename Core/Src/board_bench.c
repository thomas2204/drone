#include "board_bench.h"
#include "cmsis_os2.h"
#include "fmc.h"
#include "usart.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SDRAM_BASE 0xC0000000UL
#define SDRAM_BYTES (32UL * 1024UL * 1024UL)

volatile BoardBenchReport board_bench;
volatile uint32_t board_bench_test_bytes = SDRAM_BYTES;
volatile uint32_t board_bench_manual_a11;
static void Log(const char *message)
{
    (void)HAL_UART_Transmit(&huart7, (const uint8_t *)message,
                           (uint16_t)strlen(message), 500);
}

void BoardBench_ConfigureMPU(void)
{
    MPU_Region_InitTypeDef region = {0};
    HAL_MPU_Disable();
    region.Enable = MPU_REGION_ENABLE;
    region.Number = MPU_REGION_NUMBER1;
    region.BaseAddress = SDRAM_BASE;
    region.Size = MPU_REGION_SIZE_32MB;
    region.AccessPermission = MPU_REGION_FULL_ACCESS;
    region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    /* Normal, noncacheable memory: reads must reach the external chip. */
    region.TypeExtField = MPU_TEX_LEVEL1;
    region.IsShareable = MPU_ACCESS_SHAREABLE;
    region.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    region.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&region);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
    __DSB();
    __ISB();
}

static bool Command(uint32_t mode, uint32_t refreshes, uint32_t mode_register)
{
    FMC_SDRAM_CommandTypeDef command = {
        .CommandMode = mode, .CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1,
        .AutoRefreshNumber = refreshes, .ModeRegisterDefinition = mode_register
    };
    board_bench.hal_status = HAL_SDRAM_SendCommand(&hsdram1, &command, 100);
    return board_bench.hal_status == HAL_OK;
}

static bool InitSDRAM(void)
{
    board_bench.stage = 1;
    /* The IOC timing is calculated for 200 MHz FMC / 2 = 100 MHz SDCLK.
     * Refuse other clocks rather than silently testing invalid timings. */
    /* This HAL version does not handle FMC in GetPeriphCLKFreq and returns
     * zero. Read the source selector first; the board uses Domain 1 HCLK. */
    board_bench.fmc_clock_hz = __HAL_RCC_GET_FMC_SOURCE() == RCC_FMCCLKSOURCE_D1HCLK
                                  ? HAL_RCC_GetHCLKFreq() : 0U;
    if (board_bench.fmc_clock_hz != 200000000U)
        return false;
    if (!Command(FMC_SDRAM_CMD_CLK_ENABLE, 1, 0)) return false;
    osDelay(2); /* At least 200 us after enabling the SDRAM clock. */
    if (!Command(FMC_SDRAM_CMD_PALL, 1, 0)) return false;
    if (!Command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8, 0)) return false;
    /* BL=1, sequential, CAS=2, standard mode, single-location writes. */
    if (!Command(FMC_SDRAM_CMD_LOAD_MODE, 1, 0x220U)) return false;
    /* floor(100 MHz * 64 ms / 8192) - FMC safety margin (20). */
    board_bench.hal_status = HAL_SDRAM_ProgramRefreshRate(&hsdram1, 761);
    return board_bench.hal_status == HAL_OK;
}

static bool Equal(uint32_t address, uint32_t expected, uint32_t actual)
{
    if (expected == actual) return true;
    board_bench.address = address;
    board_bench.expected = expected;
    board_bench.actual = actual;
    return false;
}

static uint32_t Pattern(uint32_t index, uint32_t pass)
{
    static const uint32_t constants[] = {0U, 0xFFFFFFFFU, 0xAAAAAAAAU, 0x55555555U};
    if (pass < 4) return constants[pass];
    return pass == 4 ? index : ~index;
}

static bool SelectBlock(uint32_t byte_offset)
{
    if (!board_bench.manual_a11) return true;
    /* Close the open row before changing its external A11 level. */
    __DSB();
    if (!Command(FMC_SDRAM_CMD_PALL, 1, 0)) return false;
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1,
        (byte_offset & 0x200000U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    __DSB();
    osDelay(1); /* Deliberate settling time for this diagnostic, not a driver. */
    return true;
}

static bool TestSDRAM(void)
{
    const uint32_t tested_bytes = board_bench_test_bytes;
    board_bench.tested_bytes = tested_bytes;
    if (tested_bytes < 4U || tested_bytes > SDRAM_BYTES || (tested_bytes & 3U) != 0U)
        return false;
    volatile uint16_t *half = (volatile uint16_t *)SDRAM_BASE;
    volatile uint32_t *memory = (volatile uint32_t *)SDRAM_BASE;
    board_bench.stage = 2;
    /* Walking data bits on the physical 16-bit bus. */
    for (uint32_t bit = 0; bit < 16; ++bit) {
        half[0] = (uint16_t)(1U << bit);
        __DSB();
        if (!Equal(SDRAM_BASE, 1U << bit, half[0])) return false;
        half[0] = (uint16_t)~(1U << bit);
        __DSB();
        if (!Equal(SDRAM_BASE, (uint16_t)~(1U << bit), half[0])) return false;
    }
    /* Byte writes check LDQM/UDQM independently. */
    volatile uint8_t *bytes = (volatile uint8_t *)SDRAM_BASE;
    half[0] = 0xA55AU;
    bytes[0] = 0x3CU;
    __DSB();
    if (!Equal(SDRAM_BASE, 0xA53CU, half[0])) return false;
    bytes[1] = 0xC3U;
    __DSB();
    if (!Equal(SDRAM_BASE, 0xC33CU, half[0])) return false;

    /* Whole-region writes precede reads, catching address aliasing across
     * rows/banks. Noncacheable volatile accesses prevent a cache-only pass. */
    for (uint32_t pass = 0; pass < 6; ++pass) {
        board_bench.memory_pass = pass + 1;
        Log("SDRAM: writing test region\r\n");
        for (uint32_t i = 0; i < tested_bytes / 4; ++i) {
            if ((i & 0x7FFFFU) == 0U && !SelectBlock(i * 4U)) return false;
            memory[i] = Pattern(i, pass);
            if ((i & 0xFFFFU) == 0) osDelay(1);
        }
        __DSB();
        osDelay(100); /* Idle longer than a full 64 ms refresh interval. */
        for (uint32_t i = 0; i < tested_bytes / 4; ++i) {
            if ((i & 0x7FFFFU) == 0U && !SelectBlock(i * 4U)) return false;
            if (!Equal(SDRAM_BASE + i * 4, Pattern(i, pass), memory[i])) return false;
            if ((i & 0xFFFFU) == 0) osDelay(1);
        }
        Log("SDRAM: pass complete\r\n");
    }
    return true;
}

void BoardBench_Task(void *argument)
{
    (void)argument;
    Log("\r\nBENCH: SDRAM only; application tasks disabled\r\n");
    uint32_t started = HAL_GetTick();
    bool initialized = InitSDRAM();
    board_bench.manual_a11 = board_bench_manual_a11 != 0U;
    if (initialized && board_bench.manual_a11) {
        Log("DIAGNOSTIC: software-controlled A11; not normal memory mapping\r\n");
        GPIO_InitTypeDef pin = {0};
        (void)Command(FMC_SDRAM_CMD_PALL, 1, 0);
        HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1, GPIO_PIN_RESET);
        pin.Pin = GPIO_PIN_1;
        pin.Mode = GPIO_MODE_OUTPUT_PP;
        pin.Pull = GPIO_NOPULL;
        pin.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        HAL_GPIO_Init(GPIOG, &pin);
    }
    bool memory_ok = initialized && TestSDRAM();
    if (initialized && board_bench.manual_a11) {
        GPIO_InitTypeDef pin = {0};
        (void)Command(FMC_SDRAM_CMD_PALL, 1, 0);
        pin.Pin = GPIO_PIN_1;
        pin.Mode = GPIO_MODE_AF_PP;
        pin.Pull = GPIO_NOPULL;
        pin.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        pin.Alternate = GPIO_AF12_FMC;
        HAL_GPIO_Init(GPIOG, &pin);
    }
    board_bench.sdram_ms = HAL_GetTick() - started;
    board_bench.sdram_result = memory_ok ? 1 : -1;
    Log(memory_ok ? (board_bench.manual_a11 ? "SDRAM: PASS with software A11 only\r\n"
                                          : "SDRAM: PASS\r\n")
                  : "SDRAM: FAIL; inspect board_bench\r\n");
    if (!memory_ok) {
        char message[192];
        int length = snprintf(message, sizeof(message),
            "stage=%lu pass=%lu addr=0x%08lX expected=0x%08lX actual=0x%08lX clock=%lu HAL=%lu\r\n",
            (unsigned long)board_bench.stage, (unsigned long)board_bench.memory_pass,
            (unsigned long)board_bench.address, (unsigned long)board_bench.expected,
            (unsigned long)board_bench.actual, (unsigned long)board_bench.fmc_clock_hz,
            (unsigned long)board_bench.hal_status);
        if (length > 0 && (size_t)length < sizeof(message)) Log(message);
    }
    if (memory_ok) board_bench.stage = 3;
    for (;;) {
        Log(memory_ok ? (board_bench.manual_a11 ? "BENCH: software A11 test passed; normal mapping not validated\r\n"
                                              : "BENCH: PASS; safe to power off\r\n")
                              : "BENCH: FAIL; inspect board_bench\r\n");
        osDelay(5000);
    }
}
