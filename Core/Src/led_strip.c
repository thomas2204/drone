#include "led_strip.h"
#include "tim.h"
#include "ws2812_dma.h"
#include <string.h>

/* 320 us reset low for WS2812B variants. A leading zero primes CCR preload.
 * Round allocation to full cache lines so cleaning cannot affect neighbours.
 * .dma_buffer is NOLOAD in RAM_D2, so Init must explicitly initialize it.
 */
#define RESET_SLOTS 256U
#define BUFFER_SLOTS (1U + LED_STRIP_MAX_LEDS * 24U + RESET_SLOTS)
#define BUFFER_BYTES ((BUFFER_SLOTS * sizeof(uint16_t) + 31U) & ~31U)
static uint16_t pwm[BUFFER_BYTES / sizeof(uint16_t)]
    __attribute__((section(".dma_buffer"), aligned(32)));
static WS2812_Strand strip;
static volatile uint8_t busy;

HAL_StatusTypeDef LED_Strip_Init(uint16_t count)
{
    if (busy) return HAL_BUSY;
    if (count == 0U || count > LED_STRIP_MAX_LEDS || htim1.Instance != TIM1)
        return HAL_ERROR;
    if (htim1.Init.Prescaler != 0U ||
        htim1.hdma[TIM_DMA_ID_CC3] == NULL ||
        htim1.hdma[TIM_DMA_ID_CC3]->Init.Mode != DMA_NORMAL)
        return HAL_ERROR;

    /* TIM1 is on APB2. CubeMX now runs APB2 at 100 MHz rather than
     * the old 32 MHz. Derive the timer period instead of rejecting boot
     * or transmitting the old ARR=79 at the wrong bit rate. */
    uint32_t apb_divider = RCC->D2CFGR & RCC_D2CFGR_D2PPRE2;
    uint32_t timer_clock = HAL_RCC_GetPCLK2Freq();
    if ((RCC->CFGR & RCC_CFGR_TIMPRE) == 0U) {
        if (apb_divider != RCC_APB2_DIV1) timer_clock *= 2U;
    } else {
        if (apb_divider == RCC_APB2_DIV1 || apb_divider == RCC_APB2_DIV2 ||
            apb_divider == RCC_APB2_DIV4)
            timer_clock = HAL_RCC_GetHCLKFreq();
        else timer_clock *= 4U;
    }
    uint32_t period = timer_clock / 800000U;
    if (timer_clock % 800000U != 0U || period < 3U || period > 65535U)
        return HAL_ERROR;
    htim1.Init.Period = period - 1U;
    __HAL_TIM_SET_AUTORELOAD(&htim1, htim1.Init.Period);

    memset(pwm, 0, sizeof(pwm));
    strip.htim = &htim1;
    strip.channel = TIM_CHANNEL_3;
    strip.pwmBuffer = &pwm[1];
    strip.ledCount = count;
    strip.totalSize = (uint16_t)(1U + count * 24U + RESET_SLOTS);
    return LED_Strip_Fill(0, 0, 0, 0);
}

HAL_StatusTypeDef LED_Strip_Set(uint16_t index, uint8_t r, uint8_t g,
                               uint8_t b, uint8_t brightness)
{
    if (busy) return HAL_BUSY;
    if (index >= strip.ledCount) return HAL_ERROR;
    WS2812_SetPixel(&strip, index, r, g, b, brightness);
    return HAL_OK;
}

HAL_StatusTypeDef LED_Strip_Fill(uint8_t r, uint8_t g, uint8_t b,
                                uint8_t brightness)
{
    if (busy) return HAL_BUSY;
    if (strip.ledCount == 0U) return HAL_ERROR;
    for (uint16_t i = 0; i < strip.ledCount; ++i)
        WS2812_SetPixel(&strip, i, r, g, b, brightness);
    return HAL_OK;
}

HAL_StatusTypeDef LED_Strip_Show(void)
{
    if (busy) return HAL_BUSY;
    if (strip.ledCount == 0U) return HAL_ERROR;
    busy = 1U;
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
        SCB_CleanDCache_by_Addr((uint32_t *)pwm, (int32_t)sizeof(pwm));
    __DSB();
    __HAL_TIM_SET_COUNTER(&htim1, 0U);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, 0U);
    /* Load the zero CCR before enabling DMA to avoid a stale first pulse. */
    htim1.Instance->EGR = TIM_EGR_UG;
    HAL_StatusTypeDef result = HAL_TIM_PWM_Start_DMA(
        &htim1, TIM_CHANNEL_3, (uint32_t *)pwm, strip.totalSize);
    if (result != HAL_OK) busy = 0U;
    return result;
}

uint8_t LED_Strip_IsBusy(void)
{
    return busy;
}

/* HAL invokes these with Channel still identifying the completed channel.
 * If adding another PWM DMA user, merge its callback handling here.
 */
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if (htim == &htim1 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3 && busy) {
        HAL_TIM_PWM_Stop_DMA(htim, TIM_CHANNEL_3);
        busy = 0U;
    }
}

void HAL_TIM_ErrorCallback(TIM_HandleTypeDef *htim)
{
    if (htim == &htim1 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3 && busy) {
        HAL_TIM_PWM_Stop_DMA(htim, TIM_CHANNEL_3);
        busy = 0U;
        /* A failed transfer can leave a partial colour frame on the strip.
         * The owning task can resend its complete frame with Show(). */
    }
}
