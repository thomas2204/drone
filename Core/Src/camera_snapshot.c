#include "camera_snapshot.h"
#include "video_log.h"
#if CAMERA_USE_SDRAM
#include "camera_sdram.h"
#define CAMERA_FRAME_SECTION ".camera_sdram"
#else
#define CAMERA_FRAME_SECTION ".sd_dma_buffer"
#endif
#include "camera_jpeg.h"
#include "camera_sccb.h"
#include "camera_user_table.h"
#include "camera_avi.h"
#include "camera_bgr.h"
#include "camera_flow.h"
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
#if CAMERA_USE_SDRAM
/* Keep one transfer below the DMA's 65535-word limit. The capture accounting
 * below intentionally uses single-buffer NDTR, not HAL's multi-buffer mode. */
#define CAPTURE_BYTES CAMERA_AVI_MAX_JPEG_BYTES
#else
#define CAPTURE_BYTES (128U * 1024U)
#endif
_Static_assert(CAPTURE_BYTES / 4U <= 65535U, "Capture must fit single DMA transfer");
#define IO_BYTES (CAMERA_USE_SDRAM ? 16384U : 4096U)

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
volatile uint32_t camera_snapshot_video_uncompressed = CAMERA_FAST_MJPEG ? 0U : 1U;
volatile uint32_t camera_snapshot_video_mp4 = CAMERA_FAST_MJPEG ? 0U : 1U;
volatile uint32_t camera_snapshot_export_recording;
/* Flow calibration continues to use its established VGA profile. */
volatile uint32_t camera_snapshot_jpeg_width =
    CAMERA_USE_SDRAM && !CAMERA_FLOW_BENCH ? 800U : 640U;
/* Only the frame buffers move to SDRAM; FatFs and I/O stay in AXI SRAM. */
static uint32_t capture[CAPTURE_BYTES / 4]
    __attribute__((section(CAMERA_FRAME_SECTION), aligned(32)));
static uint8_t io_buffer[IO_BYTES]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
static uint32_t raw_d2[4096]
    __attribute__((section(".dma_buffer"), aligned(32)));
static uint8_t video_bgr[320U * 240U * 3U]
    __attribute__((section(CAMERA_FRAME_SECTION), aligned(32)));
static FIL camera_file __attribute__((section(".sd_dma_buffer"), aligned(32)));
static volatile uint32_t frame_done, capture_error;
static volatile CameraSnapshotReport *volatile camera_capture_report = &camera_snapshot;
static bool commanded_recording;
static uint16_t output_width=320, output_height=240;

static bool SelectOutput(bool direct_jpeg)
{
    if (!direct_jpeg) {
        output_width = 320; output_height = 240;
        camera_snapshot.frame_period_ms = 250;
        return true;
    }
    uint32_t width = camera_snapshot_jpeg_width;
    if (width != 640U && width != 800U && width != 1024U &&
        width != 1280U && width != 1600U) return false;
#if !CAMERA_USE_SDRAM || CAMERA_FLOW_BENCH
    if (width != 640U) return false;
#endif
    output_width = (uint16_t)width;
    output_height = (uint16_t)(width * 3U / 4U);
    camera_snapshot.frame_period_ms = width >= 800U ? 100U : 67U;
#if CAMERA_FLOW_BENCH
    camera_snapshot.frame_period_ms = 50U;
#endif
    return true;
}

static void Log(const char *message)
{
    (void)AppLog_Write(message, (uint16_t)strlen(message), 500);
}

static void DelayMs(uint32_t milliseconds)
{
    uint32_t ticks = (uint32_t)(((uint64_t)milliseconds * osKernelGetTickFreq() + 999) / 1000);
    osDelay(ticks ? ticks : 1);
}

static bool SetFailure(volatile CameraSnapshotReport *report, CameraFailure failure)
{
    report->failure_stage = report->stage;
    report->failure = failure;
    return false;
}

static bool Fail(CameraFailure failure)
{
    return SetFailure(&camera_snapshot, failure);
}

static bool Register(uint8_t reg, uint8_t *value, bool read)
{
    camera_capture_report->failed_reg = reg;
    if (osMutexAcquire(i2c1MutexHandle, osWaitForever) != osOK)
        return SetFailure(camera_capture_report, CAMERA_I2C);
    /* Preserve the thermal camera's CubeMX 400 kHz bus configuration. Only
     * this STOP-delimited SCCB transaction uses the proven 100 kHz timing. */
    uint32_t saved_timing = hi2c1.Instance->TIMINGR;
    if (!camera_capture_report->software_sccb) {
        __HAL_I2C_DISABLE(&hi2c1);
        hi2c1.Instance->TIMINGR = 0x20B182BE;
        __HAL_I2C_ENABLE(&hi2c1);
    }
    /* SCCB register selection and read use separate STOP-delimited transfers.
     * Writes are one uninterrupted register/value transfer, avoiding a
     * memory-write RELOAD pause between the two bytes. */
    HAL_StatusTypeDef status;
    if (camera_capture_report->software_sccb) {
        status = CameraSCCB_Register(reg, value, read);
        camera_capture_report->sccb_nacks = camera_sccb_nacks;
    } else if (read) {
        status = HAL_I2C_Master_Transmit(&hi2c1, CAMERA_ADDRESS, &reg, 1, 100);
        if (status == HAL_OK)
            status = HAL_I2C_Master_Receive(&hi2c1, CAMERA_ADDRESS, value, 1, 100);
    } else {
        uint8_t command[2] = {reg, *value};
        status = HAL_I2C_Master_Transmit(&hi2c1, CAMERA_ADDRESS, command, 2, 100);
    }
    camera_capture_report->hal_status = status;
    camera_capture_report->i2c_error = camera_capture_report->software_sccb
        ? (status == HAL_OK ? 0U : HAL_I2C_ERROR_TIMEOUT) : HAL_I2C_GetError(&hi2c1);
    if (camera_capture_report->trace_count < 32U) {
        unsigned index = camera_capture_report->trace_count;
        camera_capture_report->trace[index][0] = reg;
        camera_capture_report->trace[index][1] = status == HAL_OK ? *value : 0xFFFFFFFFU;
        camera_capture_report->trace[index][2] = read;
        camera_capture_report->trace[index][3] = status;
        camera_capture_report->trace[index][4] = camera_capture_report->i2c_error;
        camera_capture_report->trace[index][5] = (GPIOB->IDR >> 8) & 3U;
        camera_capture_report->trace_count = index + 1U;
    }
    if (!camera_capture_report->software_sccb) {
        __HAL_I2C_DISABLE(&hi2c1);
        hi2c1.Instance->TIMINGR = saved_timing;
        __HAL_I2C_ENABLE(&hi2c1);
    }
    (void)osMutexRelease(i2c1MutexHandle);
    if (status == HAL_OK && !read) DelayMs(1);
    return status == HAL_OK ? true : SetFailure(camera_capture_report, CAMERA_I2C);
}

static bool Write(uint8_t reg, uint8_t value)
{
    return Register(reg, &value, false);
}

static bool Table(const uint8_t (*table)[2])
{
    for (unsigned i = 0; table[i][0] || table[i][1]; ++i) {
        if (!Write(table[i][0], table[i][1])) return false;
        if (camera_capture_report->probe_registers && table == ov2640_settings_cif && i < 2U) {
            uint8_t bank = 0;
            if (!Register(0xFF, &bank, true)) return false;
            camera_capture_report->bank_readback[i + 1U] = bank;
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
        if ((a ^ last_a) & GPIO_PIN_6) ++camera_capture_report->pclk_edges[sample];
        if ((a ^ last_a) & GPIO_PIN_4) ++camera_capture_report->href_edges[sample];
        if ((g ^ last_g) & GPIO_PIN_9) ++camera_capture_report->vsync_edges[sample];
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
    camera_capture_report->stage = CAMERA_SENSOR;
    /* Bus reconfiguration is restricted to the isolated diagnostic build. */
#if BOARD_CAMERA_TEST
    if (osMutexAcquire(i2c1MutexHandle, osWaitForever) != osOK) return SetFailure(camera_capture_report, CAMERA_I2C);
    HAL_StatusTypeDef bus_status = HAL_I2C_DeInit(&hi2c1);
    if (bus_status == HAL_OK) {
        if (camera_capture_report->software_sccb) bus_status = CameraSCCB_Init() ? HAL_OK : HAL_ERROR;
        else {
            hi2c1.Init.Timing = 0x20B182BE;
            bus_status = HAL_I2C_Init(&hi2c1);
            if (bus_status == HAL_OK) bus_status = HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
            if (bus_status == HAL_OK) bus_status = HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0);
        }
    }
    camera_capture_report->hal_status = bus_status;
    (void)osMutexRelease(i2c1MutexHandle);
    if (bus_status != HAL_OK) return SetFailure(camera_capture_report, CAMERA_I2C);
#else
    if (camera_capture_report->software_sccb) return SetFailure(camera_capture_report, CAMERA_I2C);
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
    camera_capture_report->sensor_id = ((uint32_t)high << 8) | low;
    if (high != 0x26 || (low != 0x41 && low != 0x42))
        return SetFailure(camera_capture_report, CAMERA_SENSOR_ID);
    Log("CAMERA: OV2640 detected\r\n");
    camera_capture_report->stage = CAMERA_CONFIGURE;
    if (!Write(0x12, 0x80)) return false;
    DelayMs(20);
    if (camera_capture_report->probe_registers == 2U) return ProbeOtherRegisters();
    if ((camera_capture_report->probe_registers >= 3U && camera_capture_report->probe_registers <= 5U) || camera_capture_report->probe_registers == 7U) {
        /* This reference outputs RGB565 at 480x272, not our 320x240 JPEG.
         * Test configuration/activity only; never pass it to JPEG capture. */
        unsigned first = camera_capture_report->probe_registers >= 4U ? 3U : 0U;
        for (unsigned i = first; i < sizeof(camera_user_480x272) / sizeof(camera_user_480x272[0]); ++i)
            if (!Write(camera_user_480x272[i][0], camera_user_480x272[i][1])) return false;
        DelayMs(500);
        ObserveOutputs(1);
        if (camera_capture_report->probe_registers == 5U) return ProbeOutputs();
        if (camera_capture_report->probe_registers == 7U) return ProbeRawCapture();
        return true;
    }
    if (camera_capture_report->probe_registers) {
        uint8_t bank = 0;
        if (!Register(0xFF, &bank, true)) return false;
        camera_capture_report->bank_readback[0] = bank;
    }
    const uint8_t (*initial)[2] = ov2640_settings_cif;
    if (camera_capture_report->skip_dsp_prefix) initial += 3;
    if (!Table(initial) || !Write(0xFF, 0) || !Write(0x05, 1) ||
        !Table(output_width > 800 ? ov2640_settings_to_uxga :
               output_width > 400 ? ov2640_settings_to_svga : ov2640_settings_to_cif)) return false;
    /* Same CIF window/scaling sequence as the Espressif driver: 400x296
     * sensor window, downscaled to QVGA 320x240; JPEG DVP divider 8. */
    static const uint8_t qvga[][2] = {
        {0xFF,0}, {0x51,100}, {0x52,74}, {0x53,0}, {0x54,0}, {0x55,0},
        {0x57,0}, {0x5A,80}, {0x5B,60}, {0x5C,0},
        {0xFF,1}, {0x11,0}, {0xFF,0}, {0xD3,8}, {0x05,0}, {0,0}
    };
    /* Native SVGA sensor window (800x600), DSP output VGA 640x480. */
    static const uint8_t vga[][2] = {
        {0xFF,0}, {0x51,200}, {0x52,150}, {0x53,0}, {0x54,0}, {0x55,0},
        {0x57,0}, {0x5A,160}, {0x5B,120}, {0x5C,0},
        {0xFF,1}, {0x11,0}, {0xFF,0}, {0xD3,8}, {0x05,0}, {0,0}
    };
    /* Full 1600x1200 sensor window, no upscaling. ZMHH carries the upper
     * output width/height bits; JPEG UXGA uses DVP divider 12. */
    static const uint8_t uxga[][2] = {
        {0xFF,0}, {0x51,0x90}, {0x52,0x2c}, {0x53,0}, {0x54,0}, {0x55,0x88},
        {0x57,0}, {0x5A,0x90}, {0x5B,0x2c}, {0x5C,0x05},
        {0xFF,1}, {0x11,0}, {0xFF,0}, {0xD3,12}, {0x05,0}, {0,0}
    };
    if (!Table(output_width > 800 ? uxga : output_width > 400 ? vga : qvga)) return false;
    if (output_width >= 800U) {
        uint16_t w = output_width / 4U, h = output_height / 4U;
        if (!Write(ZMOW, (uint8_t)w) || !Write(ZMOH, (uint8_t)h) ||
            !Write(ZMHH, (uint8_t)(((h >> 6) & 4U) | ((w >> 8) & 3U)))) return false;
    }
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
    ++camera_capture_report->frames;
    frame_done = 1;
    if ((handle->Instance->CR & DCMI_CR_CM) == DCMI_MODE_CONTINUOUS)
        __HAL_DCMI_ENABLE_IT(handle, DCMI_IT_FRAME);
}

void HAL_DCMI_VsyncEventCallback(DCMI_HandleTypeDef *handle)
{
    if (handle == &hdcmi) ++camera_capture_report->vsyncs;
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *handle)
{
    if (handle != &hdcmi) return;
    camera_capture_report->dcmi_error = HAL_DCMI_GetError(handle);
    capture_error = 1;
}

static bool Capture(uint32_t timeout_ms)
{
    camera_capture_report->stage = CAMERA_CAPTURE;
    memset(capture, 0, CAPTURE_BYTES);
    frame_done = capture_error = 0;
    uint32_t polarities = camera_snapshot_dcmi_polarities;
    hdcmi.Init.PCKPolarity = (polarities & 1U) ? DCMI_PCKPOLARITY_FALLING : DCMI_PCKPOLARITY_RISING;
    hdcmi.Init.HSPolarity = (polarities & 2U) ? DCMI_HSPOLARITY_HIGH : DCMI_HSPOLARITY_LOW;
    hdcmi.Init.VSPolarity = (polarities & 4U) ? DCMI_VSPOLARITY_LOW : DCMI_VSPOLARITY_HIGH;
    /* Snapshot completion disables error/vsync interrupts, and this HAL's
     * Stop() does not clear a previous ErrorCode. Reinitialize for retries. */
    camera_capture_report->hal_status = HAL_DCMI_Init(&hdcmi);
    if (camera_capture_report->hal_status != HAL_OK) return SetFailure(camera_capture_report, CAMERA_DMA);
    /* Direct mode prevents a partially filled DMA FIFO being mistaken for
     * captured bytes at the variable-length JPEG frame boundary. */
    if (HAL_DMA_DeInit(hdcmi.DMA_Handle) != HAL_OK) return SetFailure(camera_capture_report, CAMERA_DMA);
    hdcmi.DMA_Handle->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    hdcmi.DMA_Handle->Init.MemBurst = DMA_MBURST_SINGLE;
    if (HAL_DMA_Init(hdcmi.DMA_Handle) != HAL_OK) return SetFailure(camera_capture_report, CAMERA_DMA);
    __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_OVRRI |
                                DCMI_FLAG_ERRRI | DCMI_FLAG_VSYNCRI);
    /* This HAL normally enables FRAME only after DMA fills its requested
     * length. A JPEG is shorter than the buffer, so enable FRAME explicitly. */
    __HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_FRAME);
    camera_capture_report->hal_status = HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_CONTINUOUS,
                                                   (uint32_t)capture, CAPTURE_BYTES / 4);
    if (camera_capture_report->hal_status != HAL_OK) return SetFailure(camera_capture_report, CAMERA_DMA);
    uint32_t capture_started_ms = HAL_GetTick();
    CameraFailure failure = CAMERA_OK;
    size_t checked_bytes = 0;
    while (!capture_error) {
        const uint32_t dma_remaining = __HAL_DMA_GET_COUNTER(hdcmi.DMA_Handle);
        if (dma_remaining > CAPTURE_BYTES / 4U) { failure = CAMERA_DMA; break; }
        if (dma_remaining == 0U) { failure = CAMERA_OVERFLOW; break; }
        uint32_t received = CAPTURE_BYTES - dma_remaining * 4U;
        if (received != checked_bytes) {
            size_t offset, bytes;
            uint16_t width, height;
            __DMB();
            if (CameraJPEG_FindIncremental((const uint8_t *)capture, received, &checked_bytes,
                                           &offset, &bytes, &width, &height)) break;
        }
        if (HAL_GetTick() - capture_started_ms >= timeout_ms) { failure = CAMERA_TIMEOUT; break; }
        DelayMs(1);
    }
    if (capture_error) failure = CAMERA_DMA;
    camera_capture_report->dcmi_error = HAL_DCMI_GetError(&hdcmi);
    camera_capture_report->dma_error = HAL_DMA_GetError(hdcmi.DMA_Handle);
    uint32_t remaining = __HAL_DMA_GET_COUNTER(hdcmi.DMA_Handle);
    camera_capture_report->hal_status = HAL_DCMI_Stop(&hdcmi);
    __DSB();
    if (remaining > CAPTURE_BYTES / 4) return SetFailure(camera_capture_report, CAMERA_DMA);
    camera_capture_report->captured_bytes = CAPTURE_BYTES - remaining * 4;
    if (camera_capture_report->hal_status != HAL_OK || camera_capture_report->dcmi_error ||
        camera_capture_report->dma_error) return SetFailure(camera_capture_report, CAMERA_DMA);
    if (failure != CAMERA_OK && failure != CAMERA_OVERFLOW) return SetFailure(camera_capture_report, failure);
    /* Continuous capture can fill the buffer after a complete JPEG arrived
     * but before the task next runs. Validate the captured bytes after stop. */
    const bool buffer_full = remaining == 0U || failure == CAMERA_OVERFLOW;
    size_t offset, bytes;
    uint16_t width, height;
    if (!CameraJPEG_Find((const uint8_t *)capture, camera_capture_report->captured_bytes,
                         &offset, &bytes, &width, &height)) {
        if (buffer_full) return SetFailure(camera_capture_report, CAMERA_OVERFLOW);
        camera_capture_report->stage = CAMERA_VALIDATE;
        return SetFailure(camera_capture_report, CAMERA_JPEG);
    }
    camera_capture_report->stage = CAMERA_VALIDATE;
    camera_capture_report->jpeg_offset = (uint32_t)offset;
    camera_capture_report->jpeg_bytes = (uint32_t)bytes;
    camera_capture_report->width = width;
    camera_capture_report->height = height;
    if (width != output_width || height != output_height) return SetFailure(camera_capture_report, CAMERA_JPEG);
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

#if CAMERA_FLOW_BENCH
static void FlowTraceName(char name[20], const char *video_name)
{
    snprintf(name, 20, "0:/FLOW%.3s.CSV", video_name + sizeof("0:/VIDEO") - 1U);
}

static bool WriteFlowTrace(const void *data, size_t bytes)
{
    /* Reuse the recorder's SD-accessible buffer. This runs after capture. */
    if (bytes > IO_BYTES) return false;
    memcpy(io_buffer, data, bytes);
    UINT written = 0;
    return f_write(&camera_file, io_buffer, (UINT)bytes, &written) == FR_OK && written == bytes;
}

static void SaveFlowTrace(const char *video_name)
{
    char name[20];
    FlowTraceName(name, video_name);
    bool ok = AppStorage_Open(&camera_file, name, FA_CREATE_NEW | FA_WRITE) == FR_OK;
    if (ok) {
        ok = CameraFlow_WriteTrace(WriteFlowTrace);
        if (AppStorage_Close(&camera_file) != FR_OK) ok = false;
    }
    char message[64];
    snprintf(message, sizeof(message), ok ? "FLOW TRACE saved %s\r\n" : "FLOW TRACE save failed %s\r\n", name);
    Log(message);
}
#endif

static bool RecordVideo(void)
{
    VideoLog_Begin();
    /* First validated camera frame already exists. Capture and write one
     * bounded frame at a time; create the movie only after capture succeeds. */
    /* Only one container is active. Bound stack use as the index grows. */
    union { CameraAVI avi; CameraMP4 mp4; } container;
    CameraAVI *avi=&container.avi;
    CameraMP4 *mp4=&container.mp4;
    uint32_t hashes[CAMERA_AVI_MAX_FRAMES];
    bool opened = false, mounted = false, ok = false;
#if CAMERA_FLOW_BENCH
    const bool measure_flow = !camera_snapshot.video_mp4 && !camera_snapshot.video_uncompressed &&
        camera_snapshot.probe_registers != 6U && camera_snapshot.width == 640 && camera_snapshot.height == 480;
#endif
    char filename[20] = {0};
    if (!MountSD()) goto done;
    mounted = true;
    for (unsigned i = 0; i < 1000; ++i) {
        const char *format = camera_snapshot.video_mp4
            ? (camera_snapshot.probe_registers == 6U ? "0:/TEST%03u.MP4" : "0:/VIDEO%03u.MP4")
            : (camera_snapshot.probe_registers == 6U ? "0:/TEST%03u.AVI" : "0:/VIDEO%03u.AVI");
        snprintf(filename, sizeof(filename), format, i);
        if (camera_snapshot.probe_registers != 6U) {
            char csv[20];
            if (!VideoLog_Name(csv, filename)) { Fail(CAMERA_DATA); goto done; }
            FRESULT existing = AppStorage_Stat(csv);
            if (existing == FR_OK) continue;
            if (existing != FR_NO_FILE) { camera_snapshot.fatfs_result = existing; Fail(CAMERA_FS); goto done; }
        }
#if CAMERA_FLOW_BENCH
        if (measure_flow) {
            /* An AVI can be deleted independently of its matching flow trace. */
            char name[20];
            FlowTraceName(name, filename);
            const FRESULT trace_status = AppStorage_Stat(name);
            if (trace_status == FR_OK) {
                camera_snapshot.fatfs_result = FR_EXIST;
                continue;
            }
            if (trace_status != FR_NO_FILE) {
                camera_snapshot.fatfs_result = trace_status;
                break;
            }
        }
#endif
        camera_snapshot.fatfs_result = AppStorage_Open(&camera_file, filename, FA_CREATE_NEW | FA_WRITE);
        if (camera_snapshot.fatfs_result != FR_EXIST) break;
    }
    if (camera_snapshot.fatfs_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    opened = true;
    for (unsigned i = 0; i < sizeof(filename); ++i) camera_snapshot.filename[i] = filename[i];
    bool started = camera_snapshot.video_mp4
        ? CameraH264_Init() && CameraMP4_Begin(mp4, AVIWrite, AVISeek, NULL)
        : camera_snapshot.video_uncompressed
        ? CameraAVI_BeginBGR(avi, AVIWrite, AVISeek, NULL, camera_snapshot.width, camera_snapshot.height)
        : CameraAVI_Begin(avi, AVIWrite, AVISeek, NULL, camera_snapshot.width, camera_snapshot.height);
    if (!started) { if (camera_snapshot.failure == CAMERA_OK) Fail(CAMERA_DATA); goto done; }
#if !BOARD_CAMERA_TEST
    if (commanded_recording && !RecordControl_WaitStart(RECORD_CAMERA)) {
        Fail(record_report.failed ? CAMERA_PEER_FAILURE : CAMERA_TIMEOUT); goto done;
    }
#endif
    uint32_t start = HAL_GetTick();
    bool complete = true;
#if CAMERA_FLOW_BENCH
    if (measure_flow) CameraFlow_Begin();
#endif
    for (uint32_t i = 0; i < camera_snapshot.video_requested; ++i) {
#if !BOARD_CAMERA_TEST
        if (commanded_recording && !RecordControl_HasTime()) break;
#endif
        uint32_t frame_start = HAL_GetTick();
        if ((i || commanded_recording) && camera_snapshot.probe_registers != 6U &&
            !Capture(3000U)) { complete = false; break; }
#if CAMERA_FLOW_BENCH
        uint32_t frame_ready = HAL_GetTick();
        CameraFlowReference reference = {.imu_age_ms = UINT16_MAX, .tof_age_ms = UINT16_MAX};
#if !BOARD_CAMERA_TEST
        if (measure_flow) MAVLink_GetFlowReference(&reference);
#endif
#endif
        camera_snapshot.capture_ms += HAL_GetTick()-frame_start;
        if (camera_snapshot.probe_registers != 6U) VideoLog_Sample(i);
#if !BOARD_CAMERA_TEST
        if (commanded_recording && !RecordControl_HasTime()) break;
#endif
        camera_snapshot.stage = CAMERA_WRITE;
        const uint8_t *jpeg = (const uint8_t *)capture + camera_snapshot.jpeg_offset;
        const uint8_t *data = jpeg;
        uint32_t bytes = camera_snapshot.jpeg_bytes;
        if (camera_snapshot.video_uncompressed || camera_snapshot.video_mp4) {
            uint32_t decode_start=HAL_GetTick();
            if (!CameraBGR_Decode(jpeg, bytes, video_bgr, 320, 240)) {
                Fail(CAMERA_JPEG); goto done;
            }
            data = video_bgr; bytes = sizeof(video_bgr);
            camera_snapshot.decode_ms += HAL_GetTick()-decode_start;
        }
        if (camera_snapshot.video_mp4) {
            uint32_t encode_start=HAL_GetTick();
            if (!CameraH264_Encode(video_bgr, sizeof(video_bgr), &data, &bytes)) { Fail(CAMERA_JPEG); goto done; }
            camera_snapshot.encode_ms += HAL_GetTick()-encode_start;
            uint32_t write_start=HAL_GetTick();
            if (!CameraMP4_Frame(mp4, data, bytes)) { Fail(CAMERA_DATA); goto done; }
            camera_snapshot.write_ms += HAL_GetTick()-write_start;
            camera_snapshot.video_frames = mp4->frames;
        } else {
            uint32_t write_start=HAL_GetTick();
            hashes[i] = Hash(2166136261U, data, bytes);
            if (!CameraAVI_Frame(avi, data, bytes)) goto done;
            camera_snapshot.write_ms += HAL_GetTick()-write_start;
            camera_snapshot.video_frames = avi->frames;
        }
#if CAMERA_FLOW_BENCH
        if (measure_flow) CameraFlow_Offer(jpeg, bytes, frame_start, frame_ready, HAL_GetTick(), &reference);
#endif
        uint32_t elapsed = HAL_GetTick() - frame_start;
        if (elapsed < camera_snapshot.frame_period_ms) {
            uint32_t delay=camera_snapshot.frame_period_ms-elapsed;
            DelayMs(delay); camera_snapshot.pace_ms += delay;
        }
    }
    camera_snapshot.video_duration_ms = HAL_GetTick() - start;
#if CAMERA_FLOW_BENCH
    if (measure_flow) CameraFlow_ReportCapture();
#endif
#if !BOARD_CAMERA_TEST
    if (commanded_recording) camera_snapshot.video_duration_ms = record_report.stop_requested
        ? HAL_GetTick() - record_report.start_ms : record_report.duration_ms;
#endif
    bool finalized = camera_snapshot.video_mp4
        ? CameraMP4_End(mp4, camera_snapshot.video_duration_ms)
        : CameraAVI_End(avi, camera_snapshot.video_duration_ms);
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
    uint32_t file_bytes = camera_snapshot.video_mp4 ? mp4->position : avi->position;
    uint32_t frames = camera_snapshot.video_mp4 ? mp4->frames : avi->frames;
    if (f_size(&camera_file) != file_bytes) { Fail(CAMERA_DATA); goto done; }
    for (uint32_t i = 0; i < frames; ++i) {
        uint32_t at = camera_snapshot.video_mp4 ? mp4->offsets[i] : 220U + avi->offsets[i] + 8U;
        if (!AVISeek(NULL, at)) goto done;
        uint32_t remaining = camera_snapshot.video_mp4 ? mp4->sizes[i] : avi->sizes[i], hash = 2166136261U;
        while (remaining) {
            UINT count = IOCount(remaining), received = 0;
            camera_snapshot.fatfs_result = f_read(&camera_file, io_buffer, count, &received);
            if (camera_snapshot.fatfs_result != FR_OK || received != count) { Fail(CAMERA_FS); goto done; }
            hash = Hash(hash, io_buffer, count);
            camera_snapshot.bytes_verified += count;
            remaining -= count;
        }
        if (hash != (camera_snapshot.video_mp4 ? mp4->hashes[i] : hashes[i])) { Fail(CAMERA_DATA); goto done; }
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
    if (camera_snapshot.probe_registers != 6U) {
        camera_snapshot.close_result = AppStorage_Close(&camera_file);
        opened = false;
        if (camera_snapshot.close_result != FR_OK) { Fail(CAMERA_FS); goto done; }
        camera_snapshot.stage = CAMERA_WRITE;
        camera_snapshot.fatfs_result = VideoLog_Write(&camera_file, filename, frames,
            camera_snapshot.video_duration_ms,
#if !BOARD_CAMERA_TEST
            commanded_recording ? record_report.start_ms : start,
#else
            start,
#endif
            io_buffer, IO_BYTES);
        if (camera_snapshot.fatfs_result != FR_OK) { Fail(CAMERA_FS); goto done; }
    }
    ok = complete;
done:
    camera_snapshot.sd_error = hsd1.ErrorCode;
    if (opened) {
        camera_snapshot.close_result = AppStorage_Close(&camera_file);
        if (camera_snapshot.close_result != FR_OK) { Fail(CAMERA_FS); ok = false; }
    }
#if CAMERA_FLOW_BENCH
    if (ok && measure_flow) SaveFlowTrace(filename);
#endif
    if (mounted) {
        camera_snapshot.unmount_result = AppStorage_UnmountIfIdle();
        if (camera_snapshot.unmount_result != FR_OK) { Fail(CAMERA_FS); ok = false; }
    }
    return ok;
}

#if CAMERA_FLOW_MAVLINK
static volatile CameraSnapshotReport live_camera_report;
static struct {
    uint32_t next_ms, clockrc;
    unsigned failures;
    bool ready, fault_reported;
} live_camera;
static volatile bool live_camera_active;
static volatile uint32_t live_last_frame_ms;

static bool LiveCameraProfile(void)
{
#if PMW3901_FLOW_MAVLINK
    return false;
#else
    return !camera_snapshot_video_mp4 && !camera_snapshot_video_uncompressed &&
        !camera_snapshot_probe_registers && !camera_snapshot_software_sccb &&
        !camera_snapshot_skip_dsp_prefix;
#endif
}

static bool InitRecordingSensor(void)
{
    const bool live_profile = LiveCameraProfile();
    const uint32_t clockrc = camera_snapshot_clockrc;
    if (live_profile && live_camera.ready && live_camera.clockrc == clockrc) {
        camera_snapshot.sensor_id = live_camera_report.sensor_id;
        return true;
    }
    const bool ok = InitSensor();
    live_camera.ready = live_profile && ok;
    live_camera.clockrc = clockrc;
    if (live_camera.ready) live_camera_report.sensor_id = camera_snapshot.sensor_id;
    return ok;
}

static void StreamCamera(void)
{
#if CAMERA_USE_SDRAM
    /* Also covers the streaming call inside RecordFlowLog. */
    if (camera_sdram.result != 1) return;
#endif
    const uint32_t now = HAL_GetTick();
    if ((int32_t)(now - live_camera.next_ms) < 0) return;
    live_camera.next_ms = now + 67U;
    const uint32_t clockrc = camera_snapshot_clockrc;
    if (live_camera.clockrc != clockrc) live_camera.ready = false;
    output_width = 640U;
    output_height = 480U;
    camera_capture_report = &live_camera_report;
    if (!live_camera.ready) {
        memset((void *)&live_camera_report, 0, sizeof(live_camera_report));
        live_camera.ready = InitSensor();
        live_camera.clockrc = clockrc;
    }
    const bool captured = live_camera.ready && Capture(180U);
    camera_capture_report = &camera_snapshot;
    if (!captured) {
        if (!live_camera.ready || ++live_camera.failures >= 3U) {
            live_camera_active = false;
            live_camera.ready = false;
            live_camera.failures = 0U;
            live_camera.next_ms = HAL_GetTick() + 1000U;
            if (!live_camera.fault_reported) {
                char message[96];
                snprintf(message, sizeof(message), "CAMERA: live capture failed stage=%lu error=%lu; retrying\r\n",
                         (unsigned long)live_camera_report.failure_stage,
                         (unsigned long)live_camera_report.failure);
                Log(message);
                live_camera.fault_reported = true;
            }
        }
        return;
    }
    const uint32_t captured_ms = HAL_GetTick();
    live_last_frame_ms = captured_ms;
    CameraFlowReference reference;
    MAVLink_GetFlowReference(&reference);
    CameraFlow_Stream((const uint8_t *)capture + live_camera_report.jpeg_offset,
                      live_camera_report.jpeg_bytes, captured_ms, &reference);
    if (!live_camera_active)
        Log("CAM: live capture started\r\n");
    live_camera_active = true;
    live_camera.failures = 0U;
    live_camera.fault_reported = false;
}

typedef struct {
    uint32_t elapsed_ms;
    CameraFlowEkfSample sample;
} FlowLogRow;

static bool WriteLogBytes(FIL *file, const void *data, size_t bytes)
{
    UINT written = 0U;
    return f_write(file, data, (UINT)bytes, &written) == FR_OK && written == bytes;
}

static bool OpenFlowLog(FIL *file, char name[20], bool flight)
{
    FRESULT result = FR_EXIST;
    for (unsigned i = 0; i < 1000U && result == FR_EXIST; ++i) {
        snprintf(name, 20, "0:/%s%03u.CSV", flight ? "FLT" : "EKF", i);
        result = AppStorage_Open(file, name, FA_CREATE_NEW | FA_WRITE);
    }
    if (result == FR_OK) return true;
    char message[64];
    snprintf(message, sizeof(message), "%s LOG: SD open failed FS=%u\r\n",
             flight ? "FLIGHT" : "FLOW", (unsigned)result);
    Log(message);
    return false;
}

static bool WriteFlowLogHeader(FIL *file, bool flight)
{
    static const char header[] = "t_ms,vx_mm_s,vy_mm_s,vz_mm_s,pos_age_ms,ekf_flags,ekf_age_ms,"
        "yaw_mrad,imu_age_ms,tof_mm,tof_age_ms,flow_tx_count,fc_flow_rx_count,fc_flow_age_ms,"
        "fc_flow_x_mrad_s,fc_flow_y_mrad_s,fc_flow_quality";
    static const char flight_columns[] =
        ",armed,mode,heartbeat_fresh,roll_mrad,pitch_mrad,gx_mrad_s,gy_mrad_s,gz_mrad_s\r\n";
    static const char line_end[] = "\r\n";
    return WriteLogBytes(file, header, sizeof(header) - 1U) &&
        WriteLogBytes(file, flight ? flight_columns : line_end,
                      flight ? sizeof(flight_columns) - 1U : sizeof(line_end) - 1U);
}

static bool WriteFlowLogRows(FIL *file, const FlowLogRow *rows, unsigned capacity,
                             unsigned first, unsigned count, bool flight)
{
    bool ok = true;
    for (unsigned i = 0; i < count && ok; ++i) {
        const FlowLogRow *row = &rows[(first + i) % capacity];
        const CameraFlowEkfSample *s = &row->sample;
        const CameraFlowReference *f = &s->flow;
        char line[208];
        int n = snprintf(line, sizeof(line), "%lu,%d,%d,%d,%u,%u,%u,%d,%u,%u,%u,%lu,%lu,%u,%d,%d,%u",
            (unsigned long)row->elapsed_ms, s->vx_mm_s, s->vy_mm_s, s->vz_mm_s,
            (unsigned)s->position_age_ms, (unsigned)s->ekf_flags, (unsigned)s->ekf_age_ms,
            f->yaw_mrad, (unsigned)f->imu_age_ms, (unsigned)f->tof_mm, (unsigned)f->tof_age_ms,
            (unsigned long)f->flow_tx_count, (unsigned long)f->fc_flow_rx_count,
            (unsigned)f->fc_flow_age_ms, f->fc_flow_x_mrad_s, f->fc_flow_y_mrad_s,
            (unsigned)f->fc_flow_quality);
        if (n <= 0 || (size_t)n >= sizeof(line)) {
            ok = false;
            break;
        }
        size_t length = (size_t)n;
        n = flight ? snprintf(line + length, sizeof(line) - length, ",%u,%u,%u,%d,%d,%d,%d,%d\r\n",
                   (unsigned)s->armed, (unsigned)s->flight_mode, (unsigned)s->heartbeat_fresh,
                   f->roll_mrad, f->pitch_mrad, f->gx_mrad_s, f->gy_mrad_s, f->gz_mrad_s)
                   : snprintf(line + length, sizeof(line) - length, "\r\n");
        ok = n > 0 && (size_t)n < sizeof(line) - length && WriteLogBytes(file, line, length + (size_t)n);
    }
    return ok;
}

static bool CloseFlowLog(FIL *file, const char *name, unsigned count, bool flight, bool ok)
{
    if (AppStorage_Close(file) != FR_OK) ok = false;
    if (AppStorage_UnmountIfIdle() != FR_OK) ok = false;
    char message[76];
    snprintf(message, sizeof(message), "%s LOG: %s %s samples=%u\r\n",
             flight ? "FLIGHT" : "FLOW", ok ? "saved" : "write failed", name, count);
    Log(message);
    return ok;
}

static bool RecordFlowLog(void)
{
    enum { sample_period_ms = 100, max_samples = 100 };
    FlowLogRow rows[max_samples];
    if (!RecordControl_WaitStart(RECORD_FLOW_LOG)) return false;
    unsigned count = 0U;
    uint32_t next_sample_ms = 0U;
    while (RecordControl_HasTime()) {
        const uint32_t elapsed = HAL_GetTick() - record_report.start_ms;
        if (elapsed >= next_sample_ms && count < max_samples) {
            rows[count].elapsed_ms = elapsed;
            MAVLink_GetFlowEkfSample(&rows[count++].sample);
            next_sample_ms = (elapsed / sample_period_ms + 1U) * sample_period_ms;
        }
        if (LiveCameraProfile() && !(SCB->CCR & SCB_CCR_DC_Msk)) StreamCamera();
        DelayMs(5);
    }
    char name[20];
    if (!count || !OpenFlowLog(&camera_file, name, false)) return false;
    const bool ok = WriteFlowLogHeader(&camera_file, false) &&
        WriteFlowLogRows(&camera_file, rows, max_samples, 0U, count, false);
    return CloseFlowLog(&camera_file, name, count, false, ok);
}

void CameraSnapshot_FlightLogTask(void *argument)
{
    (void)argument;
    enum { sample_ms = 200, checkpoint_ms = 5000, capacity = 301 };
    /* Buffer unsaved rows through a temporary SD open failure. */
    static FlowLogRow rows[capacity] __attribute__((section(".sd_dma_buffer"), aligned(32)));
    static FIL file __attribute__((section(".sd_dma_buffer"), aligned(32)));
    unsigned next = 0U, count = 0U, saved = 0U;
    uint32_t start_ms = 0U, last_attempt_ms = 0U;
    bool active = false, opened = false, failed = false;
    char name[20];
    for (;;) {
        CameraFlowEkfSample sample;
        MAVLink_GetFlowEkfSample(&sample);
        const uint32_t now = HAL_GetTick();
        const bool starting = !active && sample.heartbeat_fresh && sample.armed;
        if (starting) {
            active = true;
            start_ms = now;
        }
        if (active && !failed) {
            rows[next] = (FlowLogRow){ .elapsed_ms = now - start_ms, .sample = sample };
            next = (next + 1U) % capacity;
            if (count < capacity) ++count;
        }
        /* Link loss is recorded as a gap, never interpreted as disarm. */
        const bool finished = active && sample.heartbeat_fresh && !sample.armed;
        if (active && !failed && (starting || finished || now - last_attempt_ms >= checkpoint_ms)) {
            last_attempt_ms = now;
            bool ok = true;
            if (!opened) {
                opened = OpenFlowLog(&file, name, true);
                if (opened) ok = WriteFlowLogHeader(&file, true);
            }
            if (opened) {
                const unsigned first = (next + capacity - count) % capacity;
                if (ok) ok = WriteFlowLogRows(&file, rows, capacity, first, count, true);
                if (ok) ok = f_sync(&file) == FR_OK;
                if (ok) {
                    saved += count;
                    count = 0U;
                    if (!finished) {
                        char message[76];
                        snprintf(message, sizeof(message), "FLIGHT LOG: checkpoint %s samples=%u\r\n", name, saved);
                        Log(message);
                    }
                } else {
                    CloseFlowLog(&file, name, saved, true, false);
                    opened = false;
                    failed = true;
                }
            }
        }
        if (finished) {
            if (opened) CloseFlowLog(&file, name, saved, true, true);
            active = opened = failed = false;
            next = count = saved = 0U;
        }
        DelayMs(sample_ms);
    }
}
#endif

int CameraSnapshot_FormatStatus(char *buffer, size_t size)
{
#if CAMERA_FLOW_MAVLINK
    if (LiveCameraProfile() && (camera_snapshot.done || !camera_snapshot.video_requested)) {
        CameraFlowReading reading;
        CameraFlowEkfSample telemetry;
        const bool tracked = CameraFlow_Read(&reading);
        MAVLink_GetFlowEkfSample(&telemetry);
        const CameraFlowReference *flow = &telemetry.flow;
        const bool capturing = live_camera_active &&
            (uint32_t)(HAL_GetTick() - live_last_frame_ms) < 500U;
        const bool received = telemetry.heartbeat_fresh && flow->fc_flow_age_ms < 200U &&
            flow->fc_flow_quality > 0U;
        char tof[24];
        if (flow->tof_age_ms < 200U)
            snprintf(tof, sizeof(tof), "%u mm", (unsigned)flow->tof_mm);
        else
            snprintf(tof, sizeof(tof), "unavailable");
        return snprintf(buffer, size,
            "CAM: capture %s | track %s q=%u | FC flow %s q=%u | TX=%lu | ToF %s\r\n",
            capturing ? "OK" : "waiting",
            tracked ? "OK" : "no valid frame", tracked ? (unsigned)reading.quality : 0U,
            received ? "OK" : "no fresh data", received ? (unsigned)flow->fc_flow_quality : 0U,
            (unsigned long)flow->flow_tx_count, tof);
    }
#endif
    if (!camera_snapshot.done && !camera_snapshot.video_requested)
        return snprintf(buffer,size,"CAM: idle; send record or record cam\r\n");
    if (camera_snapshot.done && camera_snapshot.failure)
        return snprintf(buffer,size,"CAM: failed stage=%lu error=%lu\r\n",
            (unsigned long)camera_snapshot.failure_stage,(unsigned long)camera_snapshot.failure);
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
#if CAMERA_FLOW_MAVLINK
        if (RecordControl_TakeRequest(RECORD_FLOW_LOG)) {
            RecordControl_Done(RECORD_FLOW_LOG, RecordFlowLog());
            continue;
        }
#endif
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
            bool direct_jpeg=!camera_snapshot.video_mp4 && !camera_snapshot.video_uncompressed;
            const bool output_valid = SelectOutput(direct_jpeg);
            camera_snapshot.video_requested = camera_snapshot_video_frames > CAMERA_AVI_MAX_FRAMES
                ? CAMERA_AVI_MAX_FRAMES : camera_snapshot_video_frames;
            for (unsigned i = 0; i < 3U; ++i) camera_snapshot.bank_readback[i] = 0xFFFFFFFFU;
            Log(camera_snapshot.video_requested
                ? (camera_snapshot.video_mp4
                    ? "\r\nCAMERA: H.264 MP4 to SD\r\n"
                    : camera_snapshot.video_uncompressed
                    ? "\r\nCAMERA: uncompressed RGB24 AVI to SD\r\n"
                    : "\r\nCAMERA: direct MJPEG AVI to SD\r\n")
                : "\r\nCAMERA: single JPEG to SD\r\n");
#if CAMERA_USE_SDRAM
            Log("CAMERA: frame buffers in lower 2 MiB SDRAM\r\n");
#endif
            bool ok;
            if (!output_valid) ok = Fail(CAMERA_RESOLUTION);
#if CAMERA_USE_SDRAM
            else if (camera_sdram.result != 1) ok = Fail(CAMERA_SDRAM);
#endif
            else if (SCB->CCR & SCB_CCR_DC_Msk) ok = Fail(CAMERA_CACHE_ENABLED);
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
            else {
#if CAMERA_FLOW_MAVLINK
                const bool sensor_ready = InitRecordingSensor();
#else
                const bool sensor_ready = InitSensor();
#endif
                ok = sensor_ready &&
                    (((camera_snapshot.probe_registers >= 2U && camera_snapshot.probe_registers <= 5U) ||
                       camera_snapshot.probe_registers == 7U) ||
                     (Capture(3000U) && (!camera_snapshot.sd_requested ||
                      (camera_snapshot.video_requested ? RecordVideo() : Save()))));
            }
            if (ok) camera_snapshot.stage = CAMERA_DONE;
            camera_snapshot.done = 1;
            char message[192];
            int length = ok && ((camera_snapshot.probe_registers >= 2U && camera_snapshot.probe_registers <= 5U) || camera_snapshot.probe_registers == 7U || camera_snapshot.probe_registers == 8U)
                ? snprintf(message, sizeof(message), "CAMERA: register/config probe %lu PASS; no capture or SD access\r\n", (unsigned long)camera_snapshot.probe_registers)
                : ok && camera_snapshot.probe_registers == 6U
                ? snprintf(message, sizeof(message), "CAMERA: SYNTHETIC SD TEST PASS %s frames=%lu; NOT camera video\r\n",
                           (const char *)camera_snapshot.filename, (unsigned long)camera_snapshot.video_frames)
                : ok && camera_snapshot.sd_requested && camera_snapshot.video_requested
                ? snprintf(message, sizeof(message), "CAMERA: VIDEO PASS %s %lux%lu frames=%lu duration=%lums FPS=%lu.%lu; saved and verified\r\n",
                           (const char *)camera_snapshot.filename,
                           (unsigned long)camera_snapshot.width,(unsigned long)camera_snapshot.height,
                           (unsigned long)camera_snapshot.video_frames,(unsigned long)camera_snapshot.video_duration_ms,
                           (unsigned long)(camera_snapshot.video_frames*1000U/camera_snapshot.video_duration_ms),
                           (unsigned long)((camera_snapshot.video_frames*10000U/camera_snapshot.video_duration_ms)%10U))
                : ok && !camera_snapshot.sd_requested
                ? snprintf(message, sizeof(message), "CAMERA: CAPTURE PASS ID=0x%04lX JPEG=%lu bytes; SD not accessed\r\n",
                           (unsigned long)camera_snapshot.sensor_id, (unsigned long)camera_snapshot.jpeg_bytes)
                : ok
                ? snprintf(message, sizeof(message), "CAMERA: PASS %s (%lu bytes saved and verified); file closed\r\n",
                           (const char *)camera_snapshot.filename, (unsigned long)camera_snapshot.jpeg_bytes)
                : snprintf(message, sizeof(message), "CAMERA: FAIL stage=%lu error=%lu ID=0x%04lX I2C=0x%lX DCMI=0x%lX DMA=0x%lX FS=%lu captured=%lu saved=%lu\r\n",
                           (unsigned long)camera_snapshot.failure_stage, (unsigned long)camera_snapshot.failure,
                           (unsigned long)camera_snapshot.sensor_id, (unsigned long)camera_snapshot.i2c_error,
                           (unsigned long)camera_snapshot.dcmi_error, (unsigned long)camera_snapshot.dma_error,
                           (unsigned long)camera_snapshot.fatfs_result,
                           (unsigned long)camera_snapshot.captured_bytes,
                           (unsigned long)camera_snapshot.video_frames);
            if (length > 0 && (size_t)length < sizeof(message)) Log(message);
            if (!ok && camera_snapshot.failure == CAMERA_I2C)
                Log("CAMERA: SCCB transfer failed; software, wiring, power and module clock remain unverified\r\n");
#if !BOARD_CAMERA_TEST
            if (commanded_recording) RecordControl_Done(RECORD_CAMERA,ok);
#endif
            commanded_recording = false;
        }
#if CAMERA_FLOW_MAVLINK
        else if (LiveCameraProfile() && !(SCB->CCR & SCB_CCR_DC_Msk)) {
            StreamCamera();
            DelayMs(5);
            continue;
        }
#endif
        DelayMs(20);
    }
}
