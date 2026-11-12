#include "camera_snapshot.h"
#include "camera_jpeg.h"
#include "camera_sccb.h"
#include "camera_user_table.h"
#include "camera_avi.h"
#include "camera_bgr.h"
#include "camera_h264.h"
#include "camera_mp4.h"
#include "app_storage.h"
#include "app_log.h"
#if !BOARD_CAMERA_TEST
#include "record_control.h"
#endif
#include "camera_storage_pattern.h"
#include "cmsis_os2.h"
#include "dcmi.h"
#include "fatfs.h"
#include "i2c.h"
#include "main.h"
#include "sdmmc.h"
#include "usart.h"
#include "ov2640_settings.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CAMERA_ADDRESS (0x30U << 1)
#define CAPTURE_BYTES (128U * 1024U)
#define IO_BYTES 4096U

volatile CameraSnapshotReport camera_snapshot;
volatile uint32_t camera_snapshot_request = BOARD_CAMERA_TEST ? 1U : 0U;
volatile uint32_t camera_snapshot_save_to_sd = 1;
volatile uint32_t camera_snapshot_software_sccb;
volatile uint32_t camera_snapshot_skip_dsp_prefix;
volatile uint32_t camera_snapshot_probe_registers;
volatile uint32_t camera_snapshot_video_frames = 40;
volatile uint32_t camera_snapshot_clockrc = 0x80U;
volatile uint32_t camera_snapshot_selftest_pattern = 0x5AU;
volatile uint32_t camera_snapshot_dcmi_polarities = 4;
volatile uint32_t camera_snapshot_video_uncompressed = 1;
volatile uint32_t camera_snapshot_video_mp4 = 1;
volatile uint32_t camera_snapshot_export_recording;
/* AXI SRAM is accessible to both DCMI DMA1 and SDMMC1 IDMA. DTCM is not. */
static uint32_t capture[CAPTURE_BYTES / 4]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
static uint8_t io_buffer[IO_BYTES]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
static uint32_t raw_d2[4096]
    __attribute__((section(".dma_buffer"), aligned(32)));
static uint8_t video_bgr[320U * 240U * 3U]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
static FIL camera_file __attribute__((section(".sd_dma_buffer"), aligned(32)));
static volatile uint32_t frame_done, capture_error;
static bool commanded_recording;

static void Log(const char *message)
{
    (void)AppLog_Write(message, (uint16_t)strlen(message), 500);
}

static void DelayMs(uint32_t milliseconds)
{
    uint32_t ticks = (uint32_t)(((uint64_t)milliseconds * osKernelGetTickFreq() + 999) / 1000);
    osDelay(ticks ? ticks : 1);
}

static bool Fail(CameraFailure failure)
{
    camera_snapshot.failure = failure;
    return false;
}

static bool Register(uint8_t reg, uint8_t *value, bool read)
{
    camera_snapshot.failed_reg = reg;
    if (osMutexAcquire(i2c1MutexHandle, osWaitForever) != osOK)
        return Fail(CAMERA_I2C);
    /* Preserve the thermal camera's CubeMX 400 kHz bus configuration. Only
     * this STOP-delimited SCCB transaction uses the proven 100 kHz timing. */
    uint32_t saved_timing = hi2c1.Instance->TIMINGR;
    if (!camera_snapshot.software_sccb) {
        __HAL_I2C_DISABLE(&hi2c1);
        hi2c1.Instance->TIMINGR = 0x20B182BE;
        __HAL_I2C_ENABLE(&hi2c1);
    }
    /* SCCB register selection and read use separate STOP-delimited transfers.
     * Writes are one uninterrupted register/value transfer, avoiding a
     * memory-write RELOAD pause between the two bytes. */
    HAL_StatusTypeDef status;
    if (camera_snapshot.software_sccb) {
        status = CameraSCCB_Register(reg, value, read);
        camera_snapshot.sccb_nacks = camera_sccb_nacks;
    } else if (read) {
        status = HAL_I2C_Master_Transmit(&hi2c1, CAMERA_ADDRESS, &reg, 1, 100);
        if (status == HAL_OK)
            status = HAL_I2C_Master_Receive(&hi2c1, CAMERA_ADDRESS, value, 1, 100);
    } else {
        uint8_t command[2] = {reg, *value};
        status = HAL_I2C_Master_Transmit(&hi2c1, CAMERA_ADDRESS, command, 2, 100);
    }
    camera_snapshot.hal_status = status;
    camera_snapshot.i2c_error = camera_snapshot.software_sccb
        ? (status == HAL_OK ? 0U : HAL_I2C_ERROR_TIMEOUT) : HAL_I2C_GetError(&hi2c1);
    if (camera_snapshot.trace_count < 32U) {
        unsigned index = camera_snapshot.trace_count;
        camera_snapshot.trace[index][0] = reg;
        camera_snapshot.trace[index][1] = status == HAL_OK ? *value : 0xFFFFFFFFU;
        camera_snapshot.trace[index][2] = read;
        camera_snapshot.trace[index][3] = status;
        camera_snapshot.trace[index][4] = camera_snapshot.i2c_error;
        camera_snapshot.trace[index][5] = (GPIOB->IDR >> 8) & 3U;
        camera_snapshot.trace_count = index + 1U;
    }
    if (!camera_snapshot.software_sccb) {
        __HAL_I2C_DISABLE(&hi2c1);
        hi2c1.Instance->TIMINGR = saved_timing;
        __HAL_I2C_ENABLE(&hi2c1);
    }
    (void)osMutexRelease(i2c1MutexHandle);
    if (status == HAL_OK && !read) DelayMs(1);
    return status == HAL_OK ? true : Fail(CAMERA_I2C);
}

static bool Write(uint8_t reg, uint8_t value)
{
    return Register(reg, &value, false);
}

static bool Table(const uint8_t (*table)[2])
{
    for (unsigned i = 0; table[i][0] || table[i][1]; ++i) {
        if (!Write(table[i][0], table[i][1])) return false;
        if (camera_snapshot.probe_registers && table == ov2640_settings_cif && i < 2U) {
            uint8_t bank = 0;
            if (!Register(0xFF, &bank, true)) return false;
            camera_snapshot.bank_readback[i + 1U] = bank;
        }
    }
    return true;
}

static void ObserveOutputs(unsigned sample)
{
    uint32_t last_a = GPIOA->IDR, last_g = GPIOG->IDR;
    uint32_t started = HAL_GetTick();
    /* Digital activity check only: this deliberately does not estimate MHz
     * from asynchronous sampling or claim analog signal quality. */
    while (HAL_GetTick() - started < 50U) {
        uint32_t a = GPIOA->IDR, g = GPIOG->IDR;
        if ((a ^ last_a) & GPIO_PIN_6) ++camera_snapshot.pclk_edges[sample];
        if ((a ^ last_a) & GPIO_PIN_4) ++camera_snapshot.href_edges[sample];
        if ((g ^ last_g) & GPIO_PIN_9) ++camera_snapshot.vsync_edges[sample];
        last_a = a; last_g = g;
    }
}

static bool ProbeOtherRegisters(void)
{
    /* Sensor COM8 AGC-enable bit and DSP JPEG quantization scale. Preserve
     * and restore each original value; avoid reserved startup registers. */
    const uint8_t banks[] = {1, 0};
    const uint8_t registers[] = {COM8, QS};
    for (unsigned i = 0; i < 2U; ++i) {
        uint8_t original = 0, observed = 0, restored = 0;
        if (!Write(0xFF, banks[i]) ||
            !Register(registers[i], &original, true)) return false;
        uint8_t trial = i == 0 ? original ^ COM8_AGC_EN
                              : (original == 20U ? 21U : 20U);
        camera_snapshot.register_values[i][0] = original;
        camera_snapshot.register_values[i][1] = trial;
        if (!Write(registers[i], trial) ||
            !Register(registers[i], &observed, true)) return false;
        camera_snapshot.register_values[i][2] = observed;
        if (!Write(registers[i], original) ||
            !Register(registers[i], &restored, true)) return false;
        camera_snapshot.register_values[i][3] = restored;
        if (observed != trial || restored != original)
            return Fail(CAMERA_REGISTER_MISMATCH);
        ++camera_snapshot.register_checks;
    }
    uint8_t high = 0, low = 0;
    if (!Write(0xFF, 1) || !Register(0x0A, &high, true) ||
        !Register(0x0B, &low, true)) return false;
    return (((uint32_t)high << 8) | low) == camera_snapshot.sensor_id
        ? true : Fail(CAMERA_SENSOR_ID);
}

static bool ProbeOutputs(void)
{
    const uint8_t sensor_regs[] = {0x09, 0x0C, 0x11, 0x12, 0x15};
    const uint8_t dsp_regs[] = {0x05, 0xD3, 0xDA, 0xE0, 0x5A, 0x5B, 0x5C};
    for (unsigned bank = 0; bank < 2U; ++bank) {
        if (!Write(0xFF, bank == 0 ? 1 : 0)) return false;
        const uint8_t *regs = bank == 0 ? sensor_regs : dsp_regs;
        unsigned count = bank == 0 ? sizeof(sensor_regs) : sizeof(dsp_regs);
        for (unsigned i = 0; i < count; ++i) {
            uint8_t value = 0;
            if (!Register(regs[i], &value, true)) return false;
            camera_snapshot.output_registers[(bank == 0 ? 0 : 5) + i] = value;
        }
    }
    /* Probe the MCU inputs directly, independent of DCMI capture setup. */
    GPIO_InitTypeDef inputs = {0};
    inputs.Mode = GPIO_MODE_INPUT;
    inputs.Pull = GPIO_NOPULL;
    inputs.Pin = GPIO_PIN_4 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &inputs);
    inputs.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOG, &inputs);
    bool ok = false;
    /* Bypass DSP, release DSP/DVP reset, ensure normal sensor mode and
     * continuous pixel clock. Then exercise documented sensor dividers. */
    if (!Write(0xE0, 0) || !Write(0x05, 1) || !Write(0xFF, 1) ||
        !Write(0x09, (uint8_t)camera_snapshot.output_registers[0] & ~0x14U) ||
        !Write(0x0C, (uint8_t)camera_snapshot.output_registers[1] & ~1U) ||
        !Write(0x15, (uint8_t)camera_snapshot.output_registers[4] & ~0x20U)) goto restore_inputs;
    const uint8_t clocks[] = {0, 1, 3, 7, 0x80, 0x83};
    for (unsigned i = 0; i < sizeof(clocks); ++i) {
        if (!Write(0x11, clocks[i])) goto restore_inputs;
        DelayMs(100);
        uint32_t a = GPIOA->IDR, g = GPIOG->IDR;
        uint32_t start = HAL_GetTick();
        while (HAL_GetTick() - start < 500U) {
            uint32_t next_a = GPIOA->IDR, next_g = GPIOG->IDR;
            if ((a ^ next_a) & GPIO_PIN_6) ++camera_snapshot.output_activity[i][0];
            if ((a ^ next_a) & GPIO_PIN_4) ++camera_snapshot.output_activity[i][1];
            if ((g ^ next_g) & GPIO_PIN_9) ++camera_snapshot.output_activity[i][2];
            a = next_a; g = next_g;
        }
    }
    ok = true;
restore_inputs:
    /* Restore DCMI pin functions even if a probe transaction fails. */
    inputs.Mode = GPIO_MODE_AF_PP;
    inputs.Alternate = GPIO_AF13_DCMI;
    inputs.Pin = GPIO_PIN_4 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &inputs);
    inputs.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOG, &inputs);
    return ok;
}

static bool ProbeRawCapture(void)
{
    DMA_HandleTypeDef *dma = hdcmi.DMA_Handle;
    DMA_Stream_TypeDef *stream = (DMA_Stream_TypeDef *)dma->Instance;
    DMA_InitTypeDef saved = dma->Init;
    /* Exercise the actual DCMI DMA stream and each candidate destination,
     * rather than inferring accessibility from CPU or SDMMC transfers. */
    for (unsigned region = 0; region < 2U; ++region) {
        uint32_t *destination = region == 0 ? capture : raw_d2;
        uint32_t *source = (uint32_t *)io_buffer;
        for (unsigned i = 0; i < IO_BYTES / 4; ++i) {
            source[i] = 0xA55A0000U ^ (i * 0x10203U);
            destination[i] = 0;
        }
        __DSB();
        if (HAL_DMA_DeInit(dma) != HAL_OK) return Fail(CAMERA_DMA);
        dma->Init = saved;
        dma->Init.Direction = DMA_MEMORY_TO_MEMORY;
        dma->Init.Request = DMA_REQUEST_MEM2MEM;
        dma->Init.PeriphInc = DMA_PINC_ENABLE;
        dma->Init.FIFOMode = DMA_FIFOMODE_ENABLE;
        dma->Init.MemBurst = DMA_MBURST_SINGLE;
        dma->Init.PeriphBurst = DMA_PBURST_SINGLE;
        if (HAL_DMA_Init(dma) != HAL_OK ||
            HAL_DMA_Start(dma, (uint32_t)source, (uint32_t)destination, IO_BYTES / 4) != HAL_OK ||
            HAL_DMA_PollForTransfer(dma, HAL_DMA_FULL_TRANSFER, 100) != HAL_OK) return Fail(CAMERA_DMA);
        __DSB();
        if (memcmp(source, destination, IO_BYTES)) return Fail(CAMERA_DATA);
        camera_snapshot.dma_memory_pass[region] = 1;
    }
    if (HAL_DMA_DeInit(dma) != HAL_OK) return Fail(CAMERA_DMA);
    dma->Init = saved;
    dma->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    dma->Init.MemBurst = DMA_MBURST_SINGLE;
    if (HAL_DMA_Init(dma) != HAL_OK) return Fail(CAMERA_DMA);
    /* Use the supplied RGB565 profile but shrink its output to 160x120.
     * This is a byte-arrival test, independent of JPEG parsing or FRAME IRQ. */
    if (!Write(0xFF, 0) || !Write(0xE0, 4) || !Write(0x5A, 40) ||
        !Write(0x5B, 30) || !Write(0x5C, 0) || !Write(0xDA, 9) ||
        !Write(0xE0, 0) || !Write(0xFF, 1) || !Write(0x11, 0x80)) return false;
    DelayMs(500);
    bool received = false;
    for (unsigned trial = 0; trial < 8U; ++trial) {
        uint32_t *destination = (trial & 1U) ? raw_d2 : capture;
        memset(destination, 0xA5, sizeof(raw_d2));
        hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
        hdcmi.Init.PCKPolarity = (trial & 1U) ? DCMI_PCKPOLARITY_FALLING : DCMI_PCKPOLARITY_RISING;
        hdcmi.Init.HSPolarity = (trial & 2U) ? DCMI_HSPOLARITY_HIGH : DCMI_HSPOLARITY_LOW;
        hdcmi.Init.VSPolarity = (trial & 4U) ? DCMI_VSPOLARITY_LOW : DCMI_VSPOLARITY_HIGH;
        if (HAL_DCMI_Init(&hdcmi) != HAL_OK) return Fail(CAMERA_DMA);
        frame_done = capture_error = 0;
        __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_OVRRI | DCMI_FLAG_ERRRI | DCMI_FLAG_VSYNCRI);
        camera_snapshot.raw_trials[trial][0] = (uint32_t)destination;
        HAL_StatusTypeDef status = HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_CONTINUOUS,
            (uint32_t)destination, sizeof(raw_d2) / 4U);
        camera_snapshot.raw_trials[trial][1] = status;
        camera_snapshot.raw_trials[trial][2] = hdcmi.Instance->CR;
        camera_snapshot.raw_trials[trial][3] = stream->CR;
        camera_snapshot.raw_trials[trial][4] = stream->PAR;
        camera_snapshot.raw_trials[trial][5] = stream->M0AR;
        if (status == HAL_OK) {
            uint32_t start = HAL_GetTick();
            while (stream->NDTR && !capture_error && HAL_GetTick() - start < 1000U) DelayMs(1);
        }
        camera_snapshot.raw_trials[trial][6] = stream->NDTR;
        if (status == HAL_OK && stream->NDTR < sizeof(raw_d2) / 4U) received = true;
        camera_snapshot.raw_trials[trial][7] = hdcmi.Instance->RISR;
        camera_snapshot.raw_trials[trial][8] = HAL_DMA_GetError(dma);
        (void)HAL_DCMI_Stop(&hdcmi);
    }
    hdcmi.Init.JPEGMode = DCMI_JPEG_ENABLE;
    hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
    hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
    hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_HIGH;
    (void)HAL_DCMI_Init(&hdcmi);
    return received ? true : Fail(CAMERA_TIMEOUT);
}

static void ProbeDelay(void)
{
    uint32_t start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < SystemCoreClock / 100000U) __NOP();
}

static void ProbeLevel(GPIO_TypeDef *port, unsigned pin, bool high)
{
    MODIFY_REG(port->PUPDR, 3U << (pin * 2U), (high ? 1U : 2U) << (pin * 2U));
}

static void ProbePixelClock(void)
{
    ProbeLevel(GPIOA, 6, false);
    ProbeDelay();
    ProbeLevel(GPIOA, 6, true);
    ProbeDelay();
}

static bool ProbeDCMIInput(void)
{
    /* Power-down normally tristates OV2640 outputs. Toggle weak internal
     * pulls with AF13 kept selected, so DCMI input routing remains intact. */
    HAL_GPIO_WritePin(CAM_RESET_N_GPIO_Port, CAM_RESET_N_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CAM_PWDN_GPIO_Port, CAM_PWDN_Pin, GPIO_PIN_SET);
    DelayMs(20);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t before = DWT->CYCCNT;
    for (volatile unsigned i = 0; i < 128U; ++i) __NOP();
    if (DWT->CYCCNT == before) return Fail(CAMERA_DMA);
    GPIO_InitTypeDef pins = {0};
    pins.Mode = GPIO_MODE_AF_PP; pins.Pull = GPIO_PULLUP; pins.Alternate = GPIO_AF13_DCMI;
    GPIOA->BSRR = GPIO_PIN_4 | GPIO_PIN_6;
    GPIOD->BSRR = GPIO_PIN_3;
    GPIOE->BSRR = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
    ProbeLevel(GPIOG, 9, true);
    GPIOH->BSRR = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    pins.Pin = GPIO_PIN_4 | GPIO_PIN_6; HAL_GPIO_Init(GPIOA, &pins);
    pins.Pin = GPIO_PIN_3; HAL_GPIO_Init(GPIOD, &pins);
    pins.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6; HAL_GPIO_Init(GPIOE, &pins);
    pins.Pin = GPIO_PIN_9; HAL_GPIO_Init(GPIOG, &pins);
    pins.Pin = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12; HAL_GPIO_Init(GPIOH, &pins);
    uint8_t pattern = (uint8_t)camera_snapshot_selftest_pattern;
    ProbeLevel(GPIOH, 9, (pattern & 1U) != 0); ProbeLevel(GPIOH, 10, (pattern & 2U) != 0);
    ProbeLevel(GPIOH, 11, (pattern & 4U) != 0); ProbeLevel(GPIOH, 12, (pattern & 8U) != 0);
    ProbeLevel(GPIOE, 4, (pattern & 16U) != 0); ProbeLevel(GPIOD, 3, (pattern & 32U) != 0);
    ProbeLevel(GPIOE, 5, (pattern & 64U) != 0); ProbeLevel(GPIOE, 6, (pattern & 128U) != 0);
    ProbeDelay();
    camera_snapshot.selftest_gpio_a = GPIOA->IDR;
    camera_snapshot.selftest_gpio_h = GPIOH->IDR;
    camera_snapshot.selftest_gpio_data = ((GPIOH->IDR >> 9) & 15U) |
        (GPIOE->IDR & 16U) | ((GPIOD->IDR & 8U) << 2) |
        ((GPIOE->IDR & 32U) << 1) | ((GPIOE->IDR & 64U) << 1);
    memset(capture, 0, 512);
    DMA_HandleTypeDef *dma = hdcmi.DMA_Handle;
    DMA_Stream_TypeDef *stream = (DMA_Stream_TypeDef *)dma->Instance;
    (void)HAL_DMA_DeInit(dma);
    dma->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    dma->Init.MemBurst = DMA_MBURST_SINGLE;
    bool ok = HAL_DMA_Init(dma) == HAL_OK;
    hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
    hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
    hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
    hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_HIGH;
    if (ok) ok = HAL_DCMI_Init(&hdcmi) == HAL_OK;
    frame_done = capture_error = 0;
    __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_OVRRI | DCMI_FLAG_ERRRI | DCMI_FLAG_VSYNCRI);
    if (ok) ok = HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_CONTINUOUS, (uint32_t)capture, 128) == HAL_OK;
    if (ok) {
        ProbeLevel(GPIOA, 4, false);
        for (unsigned i = 0; i < 32U; ++i) ProbePixelClock();
        ProbeLevel(GPIOG, 9, false);
        for (unsigned line = 0; line < 4U; ++line) {
            ProbeLevel(GPIOA, 4, false);
            for (unsigned i = 0; i < 16U; ++i) ProbePixelClock();
            ProbeLevel(GPIOA, 4, true);
            for (unsigned i = 0; i < 256U; ++i) ProbePixelClock();
        }
        ProbeLevel(GPIOA, 4, false);
        ProbeLevel(GPIOG, 9, true);
        for (unsigned i = 0; i < 32U; ++i) ProbePixelClock();
        DelayMs(2);
        camera_snapshot.dcmi_selftest_bytes = (128U - stream->NDTR) * 4U;
        camera_snapshot.dcmi_selftest_word = capture[0];
        for (unsigned i = 0; i < 128U; ++i)
            if (capture[i] == (uint32_t)pattern * 0x01010101U) ++camera_snapshot.dcmi_selftest_matches;
    }
    (void)HAL_DCMI_Stop(&hdcmi);
    pins.Mode = GPIO_MODE_AF_PP; pins.Pull = GPIO_NOPULL; pins.Alternate = GPIO_AF13_DCMI;
    pins.Pin = GPIO_PIN_4 | GPIO_PIN_6; HAL_GPIO_Init(GPIOA, &pins);
    pins.Pin = GPIO_PIN_3; HAL_GPIO_Init(GPIOD, &pins);
    pins.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6; HAL_GPIO_Init(GPIOE, &pins);
    pins.Pin = GPIO_PIN_9; HAL_GPIO_Init(GPIOG, &pins);
    pins.Pin = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12; HAL_GPIO_Init(GPIOH, &pins);
    hdcmi.Init.JPEGMode = DCMI_JPEG_ENABLE;
    (void)HAL_DCMI_Init(&hdcmi);
    if (!ok) return Fail(CAMERA_DMA);
    if (camera_snapshot.dcmi_selftest_bytes != 512U) return Fail(CAMERA_TIMEOUT);
    return camera_snapshot.dcmi_selftest_matches == 128U ? true : Fail(CAMERA_DATA);
}

static bool InitSensor(void)
{
    camera_snapshot.stage = CAMERA_SENSOR;
    /* Bus reconfiguration is restricted to the isolated diagnostic build. */
#if BOARD_CAMERA_TEST
    if (osMutexAcquire(i2c1MutexHandle, osWaitForever) != osOK) return Fail(CAMERA_I2C);
    HAL_StatusTypeDef bus_status = HAL_I2C_DeInit(&hi2c1);
    if (bus_status == HAL_OK) {
        if (camera_snapshot.software_sccb) bus_status = CameraSCCB_Init() ? HAL_OK : HAL_ERROR;
        else {
            hi2c1.Init.Timing = 0x20B182BE;
            bus_status = HAL_I2C_Init(&hi2c1);
            if (bus_status == HAL_OK) bus_status = HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
            if (bus_status == HAL_OK) bus_status = HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0);
        }
    }
    camera_snapshot.hal_status = bus_status;
    (void)osMutexRelease(i2c1MutexHandle);
    if (bus_status != HAL_OK) return Fail(CAMERA_I2C);
#else
    if (camera_snapshot.software_sccb) return Fail(CAMERA_I2C);
#endif
    HAL_GPIO_WritePin(CAM_PWDN_GPIO_Port, CAM_PWDN_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(CAM_RESET_N_GPIO_Port, CAM_RESET_N_Pin, GPIO_PIN_RESET);
    DelayMs(10);
    HAL_GPIO_WritePin(CAM_PWDN_GPIO_Port, CAM_PWDN_Pin, GPIO_PIN_RESET);
    DelayMs(10);
    HAL_GPIO_WritePin(CAM_RESET_N_GPIO_Port, CAM_RESET_N_Pin, GPIO_PIN_SET);
    DelayMs(20);
    ObserveOutputs(0);
    uint8_t high, low;
    if (!Write(0xFF, 1) || !Register(0x0A, &high, true) ||
        !Register(0x0B, &low, true)) return false;
    camera_snapshot.sensor_id = ((uint32_t)high << 8) | low;
    if (high != 0x26 || (low != 0x41 && low != 0x42))
        return Fail(CAMERA_SENSOR_ID);
    Log("CAMERA: OV2640 detected\r\n");
    camera_snapshot.stage = CAMERA_CONFIGURE;
    if (!Write(0x12, 0x80)) return false;
    DelayMs(20);
    if (camera_snapshot.probe_registers == 2U) return ProbeOtherRegisters();
    if ((camera_snapshot.probe_registers >= 3U && camera_snapshot.probe_registers <= 5U) || camera_snapshot.probe_registers == 7U) {
        /* This reference outputs RGB565 at 480x272, not our 320x240 JPEG.
         * Test configuration/activity only; never pass it to JPEG capture. */
        unsigned first = camera_snapshot.probe_registers >= 4U ? 3U : 0U;
        for (unsigned i = first; i < sizeof(camera_user_480x272) / sizeof(camera_user_480x272[0]); ++i)
            if (!Write(camera_user_480x272[i][0], camera_user_480x272[i][1])) return false;
        DelayMs(500);
        ObserveOutputs(1);
        if (camera_snapshot.probe_registers == 5U) return ProbeOutputs();
        if (camera_snapshot.probe_registers == 7U) return ProbeRawCapture();
        return true;
    }
    if (camera_snapshot.probe_registers) {
        uint8_t bank = 0;
        if (!Register(0xFF, &bank, true)) return false;
        camera_snapshot.bank_readback[0] = bank;
    }
    const uint8_t (*initial)[2] = ov2640_settings_cif;
    if (camera_snapshot.skip_dsp_prefix) initial += 3;
    if (!Table(initial) || !Write(0xFF, 0) || !Write(0x05, 1) ||
        !Table(ov2640_settings_to_cif)) return false;
    /* Same CIF window/scaling sequence as the Espressif driver: 400x296
     * sensor window, downscaled to QVGA 320x240; JPEG DVP divider 8. */
    static const uint8_t qvga[][2] = {
        {0xFF,0}, {0x51,100}, {0x52,74}, {0x53,0}, {0x54,0}, {0x55,0},
        {0x57,0}, {0x5A,80}, {0x5B,60}, {0x5C,0},
        {0xFF,1}, {0x11,0}, {0xFF,0}, {0xD3,8}, {0x05,0}, {0,0}
    };
    if (!Table(qvga)) return false;
    if (camera_snapshot_clockrc <= 0xFFU &&
        (!Write(0xFF, 1) || !Write(0x11, (uint8_t)camera_snapshot_clockrc) || !Write(0xFF, 0))) return false;
    /* Espressif's table ties HREF to VSYNC. STM32 DCMI needs the separate
     * JPEG data-valid HREF signal for hardware synchronization. */
    if (!Table(ov2640_settings_jpeg3) || !Write(0xDA, IMAGE_MODE_JPEG_EN) || !Write(0x44, 20)) return false;
    DelayMs(500); /* Allow exposure and white balance to settle. */
    ObserveOutputs(1);
    return true;
}

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *handle)
{
    if (handle != &hdcmi) return;
    ++camera_snapshot.frames;
    frame_done = 1;
    if ((handle->Instance->CR & DCMI_CR_CM) == DCMI_MODE_CONTINUOUS)
        __HAL_DCMI_ENABLE_IT(handle, DCMI_IT_FRAME);
}

void HAL_DCMI_VsyncEventCallback(DCMI_HandleTypeDef *handle)
{
    if (handle == &hdcmi) ++camera_snapshot.vsyncs;
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *handle)
{
    if (handle != &hdcmi) return;
    camera_snapshot.dcmi_error = HAL_DCMI_GetError(handle);
    capture_error = 1;
}

static bool Capture(void)
{
    camera_snapshot.stage = CAMERA_CAPTURE;
    memset(capture, 0, sizeof(capture));
    frame_done = capture_error = 0;
    uint32_t polarities = camera_snapshot_dcmi_polarities;
    hdcmi.Init.PCKPolarity = (polarities & 1U) ? DCMI_PCKPOLARITY_FALLING : DCMI_PCKPOLARITY_RISING;
    hdcmi.Init.HSPolarity = (polarities & 2U) ? DCMI_HSPOLARITY_HIGH : DCMI_HSPOLARITY_LOW;
    hdcmi.Init.VSPolarity = (polarities & 4U) ? DCMI_VSPOLARITY_LOW : DCMI_VSPOLARITY_HIGH;
    /* Snapshot completion disables error/vsync interrupts, and this HAL's
     * Stop() does not clear a previous ErrorCode. Reinitialize for retries. */
    camera_snapshot.hal_status = HAL_DCMI_Init(&hdcmi);
    if (camera_snapshot.hal_status != HAL_OK) return Fail(CAMERA_DMA);
    /* Direct mode prevents a partially filled DMA FIFO being mistaken for
     * captured bytes at the variable-length JPEG frame boundary. */
    if (HAL_DMA_DeInit(hdcmi.DMA_Handle) != HAL_OK) return Fail(CAMERA_DMA);
    hdcmi.DMA_Handle->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    hdcmi.DMA_Handle->Init.MemBurst = DMA_MBURST_SINGLE;
    if (HAL_DMA_Init(hdcmi.DMA_Handle) != HAL_OK) return Fail(CAMERA_DMA);
    __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_OVRRI |
                                DCMI_FLAG_ERRRI | DCMI_FLAG_VSYNCRI);
    /* This HAL normally enables FRAME only after DMA fills its requested
     * length. A JPEG is shorter than the buffer, so enable FRAME explicitly. */
    __HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_FRAME);
    camera_snapshot.hal_status = HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_CONTINUOUS,
                                                   (uint32_t)capture, CAPTURE_BYTES / 4);
    if (camera_snapshot.hal_status != HAL_OK) return Fail(CAMERA_DMA);
    uint32_t started = HAL_GetTick();
    CameraFailure failure = CAMERA_OK;
    uint32_t checked_bytes = 0;
    while (!capture_error) {
        if (__HAL_DMA_GET_COUNTER(hdcmi.DMA_Handle) == 0) { failure = CAMERA_OVERFLOW; break; }
        uint32_t received = CAPTURE_BYTES - __HAL_DMA_GET_COUNTER(hdcmi.DMA_Handle) * 4U;
        if (received != checked_bytes) {
            size_t offset, bytes;
            uint16_t width, height;
            __DMB();
            if (CameraJPEG_Find((const uint8_t *)capture, received, &offset, &bytes, &width, &height)) break;
            checked_bytes = received;
        }
        if (HAL_GetTick() - started >= 3000) { failure = CAMERA_TIMEOUT; break; }
        DelayMs(1);
    }
    if (capture_error) failure = CAMERA_DMA;
    camera_snapshot.dcmi_error = HAL_DCMI_GetError(&hdcmi);
    camera_snapshot.dma_error = HAL_DMA_GetError(hdcmi.DMA_Handle);
    uint32_t remaining = __HAL_DMA_GET_COUNTER(hdcmi.DMA_Handle);
    camera_snapshot.hal_status = HAL_DCMI_Stop(&hdcmi);
    __DSB();
    if (remaining > CAPTURE_BYTES / 4) return Fail(CAMERA_DMA);
    camera_snapshot.captured_bytes = CAPTURE_BYTES - remaining * 4;
    if (remaining == 0) return Fail(CAMERA_OVERFLOW);
    if (camera_snapshot.hal_status != HAL_OK || camera_snapshot.dcmi_error ||
        camera_snapshot.dma_error) return Fail(CAMERA_DMA);
    if (failure != CAMERA_OK) return Fail(failure);
    camera_snapshot.stage = CAMERA_VALIDATE;
    size_t offset, bytes;
    uint16_t width, height;
    if (!CameraJPEG_Find((const uint8_t *)capture, camera_snapshot.captured_bytes,
                         &offset, &bytes, &width, &height)) return Fail(CAMERA_JPEG);
    camera_snapshot.jpeg_offset = (uint32_t)offset;
    camera_snapshot.jpeg_bytes = (uint32_t)bytes;
    camera_snapshot.width = width;
    camera_snapshot.height = height;
    if (width != 320 || height != 240) return Fail(CAMERA_JPEG);
    Log("CAMERA: complete 320x240 JPEG captured in internal SRAM\r\n");
    return true;
}

static bool MountSD(void)
{
    camera_snapshot.stage = CAMERA_SD_INIT;
    camera_snapshot.fatfs_result = AppStorage_Mount();
    camera_snapshot.sd_error = HAL_SD_GetError(&hsd1);
    if (camera_snapshot.fatfs_result == FR_OK) return true;
    return Fail(camera_snapshot.fatfs_result == FR_NOT_READY ? CAMERA_SD : CAMERA_FS);
}

static bool Save(void)
{
    bool mounted = false, opened = false, ok = false;
    char filename[20] = {0};
    const uint8_t *jpeg = (const uint8_t *)capture + camera_snapshot.jpeg_offset;
    uint32_t length = camera_snapshot.jpeg_bytes;
    UINT transferred;
    FRESULT result = FR_EXIST;
    if (!MountSD()) goto done;
    mounted = true;
    camera_snapshot.stage = CAMERA_WRITE;
    for (unsigned number = 0; number < 1000; ++number) {
        snprintf(filename, sizeof(filename), "0:/PHOTO%03u.JPG", number);
        result = AppStorage_Open(&camera_file, filename, FA_CREATE_NEW | FA_WRITE);
        if (result != FR_EXIST) break;
    }
    camera_snapshot.fatfs_result = result;
    if (result != FR_OK) { Fail(CAMERA_FS); goto done; }
    opened = true;
    for (unsigned i = 0; i < sizeof(filename); ++i) camera_snapshot.filename[i] = filename[i];
    for (uint32_t offset = 0; offset < length;) {
        UINT count = length - offset > IO_BYTES ? IO_BYTES : length - offset;
        /* JPEG SOI can occur at an unaligned offset. Always give SD IDMA
         * aligned AXI memory, including the final partial sector. */
        memcpy(io_buffer, jpeg + offset, count);
        camera_snapshot.fatfs_result = f_write(&camera_file, io_buffer, count, &transferred);
        camera_snapshot.bytes_written += transferred;
        if (camera_snapshot.fatfs_result != FR_OK || transferred != count) {
            Fail(CAMERA_FS); goto done;
        }
        offset += count;
    }
    camera_snapshot.fatfs_result = f_sync(&camera_file);
    if (camera_snapshot.fatfs_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    camera_snapshot.close_result = AppStorage_Close(&camera_file);
    opened = false;
    if (camera_snapshot.close_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    camera_snapshot.stage = CAMERA_VERIFY;
    camera_snapshot.fatfs_result = AppStorage_Open(&camera_file, filename, FA_READ);
    if (camera_snapshot.fatfs_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    opened = true;
    if (f_size(&camera_file) != length) { Fail(CAMERA_DATA); goto done; }
    for (uint32_t offset = 0; offset < length;) {
        UINT count = length - offset > IO_BYTES ? IO_BYTES : length - offset;
        camera_snapshot.fatfs_result = f_read(&camera_file, io_buffer, count, &transferred);
        if (camera_snapshot.fatfs_result != FR_OK || transferred != count) {
            Fail(CAMERA_FS); goto done;
        }
        if (memcmp(io_buffer, jpeg + offset, count)) { Fail(CAMERA_DATA); goto done; }
        camera_snapshot.bytes_verified += count;
        offset += count;
    }
    ok = true;
done:
    camera_snapshot.sd_error = hsd1.ErrorCode;
    if (opened) {
        camera_snapshot.close_result = AppStorage_Close(&camera_file);
        if (camera_snapshot.close_result != FR_OK) { Fail(CAMERA_FS); ok = false; }
    }
    if (mounted) {
        camera_snapshot.unmount_result = AppStorage_UnmountIfIdle();
        if (camera_snapshot.unmount_result != FR_OK) { Fail(CAMERA_FS); ok = false; }
    }
    return ok;
}

static UINT IOCount(uint32_t remaining)
{
    UINT count = remaining > IO_BYTES ? IO_BYTES : remaining;
    UINT partial = (UINT)(f_tell(&camera_file) & 511U);
    /* FatFs consumes a partial sector before direct DMA. If a call crosses
     * that boundary, its otherwise aligned user pointer becomes unaligned.
     * Finish that partial sector in a separate call, then use aligned I/O. */
    if (partial && count > 512U - partial) count = 512U - partial;
    return count;
}

static bool AVIWrite(void *context, const void *data, uint32_t bytes)
{
    (void)context;
    const uint8_t *source = data;
    while (bytes) {
        UINT count = IOCount(bytes), written = 0;
        memcpy(io_buffer, source, count);
        camera_snapshot.fatfs_result = f_write(&camera_file, io_buffer, count, &written);
        camera_snapshot.bytes_written += written;
        if (camera_snapshot.fatfs_result != FR_OK || written != count) return Fail(CAMERA_FS);
        source += count; bytes -= count;
    }
    return true;
}

static bool AVISeek(void *context, uint32_t position)
{
    (void)context;
    camera_snapshot.fatfs_result = f_lseek(&camera_file, position);
    return camera_snapshot.fatfs_result == FR_OK ? true : Fail(CAMERA_FS);
}

static uint32_t Hash(uint32_t hash, const uint8_t *data, uint32_t bytes)
{
    while (bytes--) { hash ^= *data++; hash *= 16777619U; }
    return hash;
}

static bool RecordVideo(void)
{
    /* First validated camera frame already exists. Do not create an empty
     * movie when the sensor fails to start. Capture/write sequentially so
     * no external SDRAM or unbounded recording buffer is required. */
    CameraAVI avi;
    CameraMP4 mp4;
    uint32_t hashes[CAMERA_AVI_MAX_FRAMES];
    bool opened = false, mounted = false, ok = false;
    char filename[20] = {0};
    if (!MountSD()) goto done;
    mounted = true;
    for (unsigned i = 0; i < 1000; ++i) {
        const char *format = camera_snapshot.video_mp4
            ? (camera_snapshot.probe_registers == 6U ? "0:/TEST%03u.MP4" : "0:/VIDEO%03u.MP4")
            : (camera_snapshot.probe_registers == 6U ? "0:/TEST%03u.AVI" : "0:/VIDEO%03u.AVI");
        snprintf(filename, sizeof(filename), format, i);
        camera_snapshot.fatfs_result = AppStorage_Open(&camera_file, filename, FA_CREATE_NEW | FA_WRITE);
        if (camera_snapshot.fatfs_result != FR_EXIST) break;
    }
    if (camera_snapshot.fatfs_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    opened = true;
    for (unsigned i = 0; i < sizeof(filename); ++i) camera_snapshot.filename[i] = filename[i];
    bool started = camera_snapshot.video_mp4
        ? CameraH264_Init() && CameraMP4_Begin(&mp4, AVIWrite, AVISeek, NULL)
        : camera_snapshot.video_uncompressed
        ? CameraAVI_BeginBGR(&avi, AVIWrite, AVISeek, NULL, 320, 240)
        : CameraAVI_Begin(&avi, AVIWrite, AVISeek, NULL, 320, 240);
    if (!started) { if (camera_snapshot.failure == CAMERA_OK) Fail(CAMERA_DATA); goto done; }
#if !BOARD_CAMERA_TEST
    if (commanded_recording && !RecordControl_WaitStart(RECORD_CAMERA)) {
        Fail(CAMERA_TIMEOUT); goto done;
    }
#endif
    uint32_t start = HAL_GetTick();
    bool complete = true;
    for (uint32_t i = 0; i < camera_snapshot.video_requested; ++i) {
#if !BOARD_CAMERA_TEST
        if (commanded_recording && !RecordControl_HasTime()) break;
#endif
        uint32_t frame_start = HAL_GetTick();
        if ((i || commanded_recording) && camera_snapshot.probe_registers != 6U && !Capture()) { complete = false; break; }
#if !BOARD_CAMERA_TEST
        if (commanded_recording && !RecordControl_HasTime()) break;
#endif
        camera_snapshot.stage = CAMERA_WRITE;
        const uint8_t *jpeg = (const uint8_t *)capture + camera_snapshot.jpeg_offset;
        const uint8_t *data = jpeg;
        uint32_t bytes = camera_snapshot.jpeg_bytes;
        if (camera_snapshot.video_uncompressed || camera_snapshot.video_mp4) {
            if (!CameraBGR_Decode(jpeg, bytes, video_bgr, 320, 240)) {
                Fail(CAMERA_JPEG); goto done;
            }
            data = video_bgr; bytes = sizeof(video_bgr);
        }
        if (camera_snapshot.video_mp4) {
            if (!CameraH264_Encode(video_bgr, sizeof(video_bgr), &data, &bytes)) { Fail(CAMERA_JPEG); goto done; }
            if (!CameraMP4_Frame(&mp4, data, bytes)) { Fail(CAMERA_DATA); goto done; }
            camera_snapshot.video_frames = mp4.frames;
        } else {
            hashes[i] = Hash(2166136261U, data, bytes);
            if (!CameraAVI_Frame(&avi, data, bytes)) goto done;
            camera_snapshot.video_frames = avi.frames;
        }
        uint32_t elapsed = HAL_GetTick() - frame_start;
        if (elapsed < 250U) DelayMs(250U - elapsed);
    }
    camera_snapshot.video_duration_ms = HAL_GetTick() - start;
#if !BOARD_CAMERA_TEST
    if (commanded_recording) camera_snapshot.video_duration_ms = record_report.duration_ms;
#endif
    bool finalized = camera_snapshot.video_mp4
        ? CameraMP4_End(&mp4, camera_snapshot.video_duration_ms)
        : CameraAVI_End(&avi, camera_snapshot.video_duration_ms);
    if (!finalized) { Fail(CAMERA_FS); goto done; }
    camera_snapshot.fatfs_result = f_sync(&camera_file);
    if (camera_snapshot.fatfs_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    camera_snapshot.close_result = AppStorage_Close(&camera_file);
    opened = false;
    if (camera_snapshot.close_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    /* Read every frame payload back and compare its hash before reporting a
     * recording pass. A capture failure still leaves finalized prior frames. */
    camera_snapshot.stage = CAMERA_VERIFY;
    camera_snapshot.fatfs_result = AppStorage_Open(&camera_file, filename, FA_READ);
    if (camera_snapshot.fatfs_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    opened = true;
    uint32_t file_bytes = camera_snapshot.video_mp4 ? mp4.position : avi.position;
    uint32_t frames = camera_snapshot.video_mp4 ? mp4.frames : avi.frames;
    if (f_size(&camera_file) != file_bytes) { Fail(CAMERA_DATA); goto done; }
    for (uint32_t i = 0; i < frames; ++i) {
        uint32_t at = camera_snapshot.video_mp4 ? mp4.offsets[i] : 220U + avi.offsets[i] + 8U;
        if (!AVISeek(NULL, at)) goto done;
        uint32_t remaining = camera_snapshot.video_mp4 ? mp4.sizes[i] : avi.sizes[i], hash = 2166136261U;
        while (remaining) {
            UINT count = IOCount(remaining), received = 0;
            camera_snapshot.fatfs_result = f_read(&camera_file, io_buffer, count, &received);
            if (camera_snapshot.fatfs_result != FR_OK || received != count) { Fail(CAMERA_FS); goto done; }
            hash = Hash(hash, io_buffer, count);
            camera_snapshot.bytes_verified += count;
            remaining -= count;
        }
        if (hash != (camera_snapshot.video_mp4 ? mp4.hashes[i] : hashes[i])) { Fail(CAMERA_DATA); goto done; }
    }
    if ((camera_snapshot.probe_registers == 6U || camera_snapshot_export_recording) && file_bytes <= CAPTURE_BYTES) {
        /* Allow ST-Link to export the actual SD test file for independent
         * container/decoder checks. Never replace real captured frames here. */
        if (!AVISeek(NULL, 0)) goto done;
        for (uint32_t offset = 0; offset < file_bytes;) {
            UINT count = file_bytes - offset > IO_BYTES ? IO_BYTES : file_bytes - offset;
            UINT received = 0;
            camera_snapshot.fatfs_result = f_read(&camera_file, io_buffer, count, &received);
            if (camera_snapshot.fatfs_result != FR_OK || received != count) { Fail(CAMERA_FS); goto done; }
            memcpy((uint8_t *)capture + offset, io_buffer, count);
            offset += count;
        }
        camera_snapshot.captured_bytes = file_bytes;
    }
    ok = complete;
#if !BOARD_CAMERA_TEST
    if (commanded_recording && record_report.failed) ok = Fail(CAMERA_TIMEOUT);
#endif
done:
    camera_snapshot.sd_error = hsd1.ErrorCode;
    if (opened) {
        camera_snapshot.close_result = AppStorage_Close(&camera_file);
        if (camera_snapshot.close_result != FR_OK) { Fail(CAMERA_FS); ok = false; }
    }
    if (mounted) {
        camera_snapshot.unmount_result = AppStorage_UnmountIfIdle();
        if (camera_snapshot.unmount_result != FR_OK) { Fail(CAMERA_FS); ok = false; }
    }
    return ok;
}

int CameraSnapshot_FormatStatus(char *buffer, size_t size)
{
    if (!camera_snapshot.done && !camera_snapshot.video_requested)
        return snprintf(buffer,size,"CAM: idle; send record\r\n");
    if (camera_snapshot.done && camera_snapshot.failure)
        return snprintf(buffer,size,"CAM: failed stage=%lu error=%lu\r\n",
            (unsigned long)camera_snapshot.stage,(unsigned long)camera_snapshot.failure);
    if (camera_snapshot.done)
        return snprintf(buffer,size,"CAM: saved %s frames=%lu\r\n",
            (const char *)camera_snapshot.filename,(unsigned long)camera_snapshot.video_frames);
    if (commanded_recording && camera_snapshot.video_frames)
        return snprintf(buffer,size,"CAM: recording 10s frames=%lu\r\n",
            (unsigned long)camera_snapshot.video_frames);
    if (camera_snapshot.video_frames)
        return snprintf(buffer,size,"CAM: recording %lu/%lu\r\n",
            (unsigned long)camera_snapshot.video_frames,(unsigned long)camera_snapshot.video_requested);
    return snprintf(buffer,size,"CAM: starting\r\n");
}

void CameraSnapshot_Task(void *argument)
{
    (void)argument;
#if !BOARD_CAMERA_TEST
    DelayMs(1500); /* Let application reception and audio tasks start first. */
#endif
    for (;;) {
#if !BOARD_CAMERA_TEST
        if (RecordControl_TakeRequest(RECORD_CAMERA)) {
            commanded_recording = true;
            camera_snapshot_video_frames = CAMERA_AVI_MAX_FRAMES;
            camera_snapshot_request = 1;
        }
#endif
        if (camera_snapshot_request) {
            camera_snapshot_request = 0;
            memset((void *)&camera_snapshot, 0, sizeof(camera_snapshot));
            camera_snapshot.sd_requested = camera_snapshot_save_to_sd != 0;
            camera_snapshot.software_sccb = camera_snapshot_software_sccb != 0;
            camera_snapshot.skip_dsp_prefix = camera_snapshot_skip_dsp_prefix != 0;
            camera_snapshot.probe_registers = camera_snapshot_probe_registers;
            camera_snapshot.video_uncompressed = camera_snapshot_video_uncompressed != 0;
            camera_snapshot.video_mp4 = camera_snapshot_video_mp4 != 0;
            camera_snapshot.video_requested = camera_snapshot_video_frames > CAMERA_AVI_MAX_FRAMES
                ? CAMERA_AVI_MAX_FRAMES : camera_snapshot_video_frames;
            for (unsigned i = 0; i < 3U; ++i) camera_snapshot.bank_readback[i] = 0xFFFFFFFFU;
            Log(camera_snapshot.video_requested
                ? (camera_snapshot.video_mp4
                    ? "\r\nCAMERA: H.264 MP4 to SD; internal SRAM\r\n"
                    : camera_snapshot.video_uncompressed
                    ? "\r\nCAMERA: uncompressed RGB24 AVI to SD; internal SRAM\r\n"
                    : "\r\nCAMERA: MJPEG AVI to SD; internal SRAM\r\n")
                : "\r\nCAMERA: single JPEG to SD; internal SRAM\r\n");
            bool ok;
            if (SCB->CCR & SCB_CCR_DC_Msk) ok = Fail(CAMERA_CACHE_ENABLED);
            else if (camera_snapshot.probe_registers == 8U) ok = ProbeDCMIInput();
            else if (camera_snapshot.probe_registers == 6U) {
                Log("CAMERA: SYNTHETIC STORAGE TEST; pattern is NOT a camera capture\r\n");
                memcpy(capture, camera_storage_pattern, sizeof(camera_storage_pattern));
                size_t offset, bytes;
                uint16_t width, height;
                ok = CameraJPEG_Find((const uint8_t *)capture, sizeof(camera_storage_pattern),
                    &offset, &bytes, &width, &height);
                if (ok) {
                    camera_snapshot.jpeg_offset = (uint32_t)offset;
                    camera_snapshot.jpeg_bytes = (uint32_t)bytes;
                    camera_snapshot.width = width;
                    camera_snapshot.height = height;
                    ok = camera_snapshot.sd_requested && camera_snapshot.video_requested && RecordVideo();
                } else Fail(CAMERA_JPEG);
            }
            else ok = InitSensor() && (((camera_snapshot.probe_registers >= 2U && camera_snapshot.probe_registers <= 5U) || camera_snapshot.probe_registers == 7U) ||
                (Capture() && (!camera_snapshot.sd_requested ||
                    (camera_snapshot.video_requested ? RecordVideo() : Save()))));
            if (ok) camera_snapshot.stage = CAMERA_DONE;
            camera_snapshot.done = 1;
            char message[192];
            int length = ok && ((camera_snapshot.probe_registers >= 2U && camera_snapshot.probe_registers <= 5U) || camera_snapshot.probe_registers == 7U || camera_snapshot.probe_registers == 8U)
                ? snprintf(message, sizeof(message), "CAMERA: register/config probe %lu PASS; no capture or SD access\r\n", (unsigned long)camera_snapshot.probe_registers)
                : ok && camera_snapshot.probe_registers == 6U
                ? snprintf(message, sizeof(message), "CAMERA: SYNTHETIC SD TEST PASS %s frames=%lu; NOT camera video\r\n",
                           (const char *)camera_snapshot.filename, (unsigned long)camera_snapshot.video_frames)
                : ok && camera_snapshot.sd_requested && camera_snapshot.video_requested
                ? snprintf(message, sizeof(message), "CAMERA: VIDEO PASS %s frames=%lu duration=%lums; saved and verified\r\n",
                           (const char *)camera_snapshot.filename, (unsigned long)camera_snapshot.video_frames,
                           (unsigned long)camera_snapshot.video_duration_ms)
                : ok && !camera_snapshot.sd_requested
                ? snprintf(message, sizeof(message), "CAMERA: CAPTURE PASS ID=0x%04lX JPEG=%lu bytes; SD not accessed\r\n",
                           (unsigned long)camera_snapshot.sensor_id, (unsigned long)camera_snapshot.jpeg_bytes)
                : ok
                ? snprintf(message, sizeof(message), "CAMERA: PASS %s (%lu bytes saved and verified); file closed\r\n",
                           (const char *)camera_snapshot.filename, (unsigned long)camera_snapshot.jpeg_bytes)
                : snprintf(message, sizeof(message), "CAMERA: FAIL stage=%lu error=%lu ID=0x%04lX I2C=0x%lX DCMI=0x%lX DMA=0x%lX FS=%lu\r\n",
                           (unsigned long)camera_snapshot.stage, (unsigned long)camera_snapshot.failure,
                           (unsigned long)camera_snapshot.sensor_id, (unsigned long)camera_snapshot.i2c_error,
                           (unsigned long)camera_snapshot.dcmi_error, (unsigned long)camera_snapshot.dma_error,
                           (unsigned long)camera_snapshot.fatfs_result);
            if (length > 0 && (size_t)length < sizeof(message)) Log(message);
            if (!ok && camera_snapshot.failure == CAMERA_I2C)
                Log("CAMERA: SCCB transfer failed; software, wiring, power and module clock remain unverified\r\n");
#if !BOARD_CAMERA_TEST
            if (commanded_recording) RecordControl_Done(RECORD_CAMERA,ok);
#endif
            commanded_recording = false;
        }
        DelayMs(20);
    }
}
