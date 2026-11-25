#include "pmw3901.h"

#include "spi.h"
#include "FreeRTOS.h"
#include "task.h"

enum {
    PRODUCT_ID = 0x00,
    MOTION = 0x02,
    MOTION_BURST = 0x16,
    POWER_UP_RESET = 0x3A,
    INVERSE_PRODUCT_ID = 0x5F,
    MOTION_PRESENT = 0x80,
    BURST_BYTES = 12,
    BURST_ADDRESS_DELAY_US = 150,
    POLL_MS = 20,
#if PMW3901_FRAME_CAPTURE
    SPI_TIMEOUT_MS = 20
#else
    SPI_TIMEOUT_MS = 5
#endif
};

typedef struct {
    uint8_t address, value;
} RegisterSetting;

/*
 * Initialization register sequence adapted from Bitcraze_PMW3901.
 * Copyright (c) 2017 Bitcraze
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/*
 * PixArt demo kit v3.20 calibration and register updates adapted from PX4.
 * Copyright (c) 2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be used to
 *    endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
static const RegisterSetting setup_before_wait[] = {
    {0x7F, 0x00}, {0x61, 0xAD}, {0x7F, 0x03}, {0x40, 0x00},
    {0x7F, 0x05}, {0x41, 0xB3}, {0x43, 0xF1}, {0x45, 0x14},
    {0x5B, 0x32}, {0x5F, 0x34}, {0x7B, 0x08}, {0x7F, 0x06},
    {0x44, 0x1B}, {0x40, 0xBF}, {0x4E, 0x3F}, {0x7F, 0x08},
    {0x65, 0x20}, {0x6A, 0x18}, {0x7F, 0x09}, {0x4F, 0xAF},
    {0x5F, 0x40}, {0x48, 0x80}, {0x49, 0x80}, {0x57, 0x77},
    {0x60, 0x78}, {0x61, 0x78}, {0x62, 0x08}, {0x63, 0x50},
    {0x7F, 0x0A}, {0x45, 0x60}, {0x7F, 0x00}, {0x4D, 0x11},
    {0x55, 0x80}, {0x74, 0x21}, {0x75, 0x1F}, {0x4A, 0x78},
    {0x4B, 0x78}, {0x44, 0x08}, {0x45, 0x50}, {0x64, 0xFF},
    {0x65, 0x1F}, {0x7F, 0x14}, {0x65, 0x67}, {0x66, 0x08},
    {0x63, 0x70}, {0x7F, 0x15}, {0x48, 0x48}, {0x7F, 0x07},
    {0x41, 0x0D}, {0x43, 0x14}, {0x4B, 0x0E}, {0x45, 0x0F},
    {0x44, 0x42}, {0x4C, 0x80}, {0x7F, 0x10}, {0x5B, 0x02},
    {0x7F, 0x07}, {0x40, 0x41}, {0x70, 0x00}
};

static const RegisterSetting setup_after_wait[] = {
    {0x32, 0x44}, {0x7F, 0x07}, {0x40, 0x40}, {0x7F, 0x06},
    {0x62, 0xF0}, {0x63, 0x00}, {0x7F, 0x0D}, {0x48, 0xC0},
    {0x6F, 0xD5}, {0x7F, 0x00}, {0x5B, 0xA0}, {0x4E, 0xA8},
    {0x5A, 0x50}, {0x40, 0x80}
};

static PMW3901Reading latest;
static uint32_t cycles_per_us;

static void WaitUs(uint32_t us)
{
    const uint32_t start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < us * cycles_per_us) {}
}

static bool Read(uint8_t address, uint8_t *data, uint16_t size)
{
    uint8_t discarded;
    uint8_t dummy[BURST_BYTES] = {0};
    HAL_GPIO_WritePin(FLOW_CS_GPIO_Port, FLOW_CS_Pin, GPIO_PIN_RESET);
    WaitUs(1);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi6, &address,
                                                      &discarded, 1, SPI_TIMEOUT_MS);
    if (status == HAL_OK) {
        /* Match the motion-burst turnaround in ArduPilot's native PixArt driver. */
        WaitUs(address == MOTION_BURST ? BURST_ADDRESS_DELAY_US : 50);
        status = HAL_SPI_TransmitReceive(&hspi6, dummy, data, size, SPI_TIMEOUT_MS);
    }
    WaitUs(1);
    HAL_GPIO_WritePin(FLOW_CS_GPIO_Port, FLOW_CS_Pin, GPIO_PIN_SET);
    WaitUs(50);
    return status == HAL_OK;
}

static bool Write(uint8_t address, uint8_t value)
{
    uint8_t data[] = { (uint8_t)(address | 0x80U), value };
    uint8_t discarded[sizeof(data)];
    HAL_GPIO_WritePin(FLOW_CS_GPIO_Port, FLOW_CS_Pin, GPIO_PIN_RESET);
    WaitUs(1);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi6, data, discarded,
                                                      sizeof(data), SPI_TIMEOUT_MS);
    WaitUs(50);
    HAL_GPIO_WritePin(FLOW_CS_GPIO_Port, FLOW_CS_Pin, GPIO_PIN_SET);
    WaitUs(200);
    return status == HAL_OK;
}

static bool WriteSettings(const RegisterSetting *settings, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (!Write(settings[i].address, settings[i].value)) return false;
    }
    return true;
}

static bool ApplySensorCalibration(void)
{
    static const RegisterSetting select_trim[] = {
        {0x7F, 0x00}, {0x55, 0x01}, {0x50, 0x07}, {0x7F, 0x0E}, {0x43, 0x10}
    };
    static const RegisterSetting select_gain[] = {
        {0x7F, 0x00}, {0x51, 0x7B}, {0x50, 0x00}, {0x55, 0x00}, {0x7F, 0x0E}
    };
    static const RegisterSetting unlock_gain[] = {
        {0x7F, 0x00}, {0x61, 0xAD}, {0x51, 0x70}, {0x7F, 0x0E}
    };
    uint8_t trim;
    if (!WriteSettings(select_trim, sizeof(select_trim) / sizeof(select_trim[0])) ||
        !Read(0x67, &trim, 1) || !Write(0x48, (trim & 0x80U) ? 0x04 : 0x02) ||
        !WriteSettings(select_gain, sizeof(select_gain) / sizeof(select_gain[0])) ||
        !Read(0x73, &trim, 1)) return false;
    if (trim != 0U) return true;

    uint8_t gain, coefficient;
    if (!Read(0x70, &gain, 1) || !Read(0x71, &coefficient, 1)) return false;
    const unsigned adjusted_gain = gain + (gain <= 28U ? 14U : 11U);
    return WriteSettings(unlock_gain, sizeof(unlock_gain) / sizeof(unlock_gain[0])) &&
           Write(0x70, adjusted_gain > 0x3FU ? 0x3FU : adjusted_gain) &&
           Write(0x71, (uint8_t)((unsigned)coefficient * 45U / 100U));
}

static bool Start(PMW3901Reading *reading)
{
    reading->product_id = 0;
    reading->inverse_id = 0;
    if (!Write(POWER_UP_RESET, 0x5A)) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!Read(PRODUCT_ID, &reading->product_id, 1) ||
        !Read(INVERSE_PRODUCT_ID, &reading->inverse_id, 1)) return false;
    if (reading->product_id != 0x49U || reading->inverse_id != 0xB6U) return true;

    for (uint8_t reg = MOTION; reg <= 0x06U; ++reg) {
        uint8_t discarded;
        if (!Read(reg, &discarded, 1)) return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    if (!ApplySensorCalibration()) return false;
    if (!WriteSettings(setup_before_wait, sizeof(setup_before_wait) / sizeof(setup_before_wait[0]))) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!WriteSettings(setup_after_wait, sizeof(setup_after_wait) / sizeof(setup_after_wait[0]))) return false;
    reading->ready = true;
    return true;
}

#if PMW3901_FRAME_CAPTURE
enum {
    FRAME_WAITING, FRAME_READING, FRAME_COMPLETE, FRAME_TIMEOUT, FRAME_IO_ERROR,
    FRAME_PIXELS = 35 * 35,
    FRAME_DATA = 0x58
};

/* Fixed layout for inspecting the one-shot capture over SWD. */
static volatile struct {
    uint32_t status, count;
    uint8_t pixels[FRAME_PIXELS];
} pmw3901_frame;

static void CaptureFrame(void)
{
    /* Frame mode register sequence from the Bitcraze driver cited above. */
    static const RegisterSetting frame_mode[] = {
        {0x7F, 0x07}, {0x41, 0x1D}, {0x4C, 0x00}, {0x7F, 0x08},
        {0x6A, 0x38}, {0x7F, 0x00}, {0x55, 0x04}, {0x40, 0x80},
        {0x4D, 0x11}, {0x70, 0x00}, {FRAME_DATA, 0xFF}
    };
    pmw3901_frame.status = FRAME_READING;
    if (!WriteSettings(frame_mode, sizeof(frame_mode) / sizeof(frame_mode[0]))) {
        pmw3901_frame.status = FRAME_IO_ERROR;
        return;
    }

    const TickType_t start = xTaskGetTickCount();
    while (xTaskGetTickCount() - start < pdMS_TO_TICKS(8000)) {
        uint8_t upper, lower;
        if (!Read(FRAME_DATA, &upper, 1)) {
            pmw3901_frame.status = FRAME_IO_ERROR;
            return;
        }
        if (upper >> 6 != 1U) continue;
        /* The frame protocol returns the remaining intensity bits on the next read. */
        if (!Read(FRAME_DATA, &lower, 1)) {
            pmw3901_frame.status = FRAME_IO_ERROR;
            return;
        }
        pmw3901_frame.pixels[pmw3901_frame.count] =
            (uint8_t)((upper << 2) | ((lower >> 2) & 0x03U));
        ++pmw3901_frame.count;
        if (pmw3901_frame.count == FRAME_PIXELS) {
            pmw3901_frame.status = FRAME_COMPLETE;
            return;
        }
    }
    pmw3901_frame.status = FRAME_TIMEOUT;
}
#endif

static void Publish(const PMW3901Reading *reading)
{
    taskENTER_CRITICAL();
    latest = *reading;
    taskEXIT_CRITICAL();
}

void PMW3901_Read(PMW3901Reading *reading)
{
    taskENTER_CRITICAL();
    *reading = latest;
    taskEXIT_CRITICAL();
}

static int16_t DecodeDelta(const uint8_t *bytes)
{
    return (int16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

void PMW3901_Task(void *argument)
{
    (void)argument;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cycles_per_us = SystemCoreClock / 1000000U;
    PMW3901Reading reading = {0};
    vTaskDelay(pdMS_TO_TICKS(50));

    for (;;) {
        /* Skip SPI if the cycle counter used for its bus delays is unavailable. */
        const uint32_t before = DWT->CYCCNT;
        __NOP();
        if (DWT->CYCCNT == before || cycles_per_us == 0U) {
            ++reading.spi_errors;
            Publish(&reading);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!Start(&reading)) ++reading.spi_errors;
        Publish(&reading);
        if (reading.ready) {
#if PMW3901_FRAME_CAPTURE
            /* Frame mode changes the sensor configuration. End motion polling until a different build is loaded. */
            CaptureFrame();
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
#endif
            TickType_t deadline = xTaskGetTickCount();
            uint32_t previous_ms = 0U;
            bool have_interval = false;
            unsigned consecutive_errors = 0;
            while (reading.ready) {
                uint8_t burst[BURST_BYTES];
                reading.capture_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                reading.valid = false;
                if (Read(MOTION_BURST, burst, sizeof(burst)) && burst[0] != 0xFFU) {
                    const bool motion = (burst[0] & MOTION_PRESENT) != 0;
                    reading.dx = motion ? DecodeDelta(&burst[2]) : 0;
                    reading.dy = motion ? DecodeDelta(&burst[4]) : 0;
#if PMW3901_BENCH_DIAGNOSTICS
                    reading.shutter = ((uint16_t)burst[10] << 8) | burst[11];
                    reading.motion = burst[0];
                    reading.observation = burst[1];
#endif
                    reading.quality = burst[6];
                    reading.interval_ms = reading.capture_ms - previous_ms;
                    reading.valid = have_interval && reading.interval_ms != 0U &&
                        reading.interval_ms <= PMW3901_MAX_SAMPLE_MS && reading.quality != 0U &&
                        !(reading.quality < 0x19U && burst[10] == 0x1FU);
                    ++reading.sequence;
                    previous_ms = reading.capture_ms;
                    have_interval = true;
                    consecutive_errors = 0;
                } else {
                    ++reading.spi_errors;
                    have_interval = false;
                    if (++consecutive_errors >= 3U) reading.ready = false;
                }
                Publish(&reading);
                if (reading.ready && xTaskDelayUntil(&deadline, pdMS_TO_TICKS(POLL_MS)) == pdFALSE) {
                    deadline = xTaskGetTickCount();
                    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
