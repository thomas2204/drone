#include "led_strip.h"
#include "ws2812_dma.h"
#include <assert.h>
#include <stdio.h>

Test_RCC test_rcc;
Test_SCB test_scb;
TIM_TypeDef test_tim1;
TIM_HandleTypeDef htim1;
static DMA_HandleTypeDef dma;
static uint32_t pclk, hclk;
uint32_t HAL_RCC_GetPCLK2Freq(void) { return pclk; }
uint32_t HAL_RCC_GetHCLKFreq(void) { return hclk; }
void SCB_CleanDCache_by_Addr(uint32_t *p, int32_t n) { (void)p; (void)n; }
void WS2812_SetPixel(WS2812_Strand *s, uint16_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t v)
{ (void)s; (void)i; (void)r; (void)g; (void)b; (void)v; }
HAL_StatusTypeDef HAL_TIM_PWM_Start_DMA(TIM_HandleTypeDef *t, uint32_t c, uint32_t *p, uint16_t n)
{ (void)t; (void)c; (void)p; assert(n == 2177); return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_PWM_Stop_DMA(TIM_HandleTypeDef *t, uint32_t c)
{ (void)t; (void)c; return HAL_OK; }
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *);

static void check_clock(uint32_t apb, uint32_t ahb, uint32_t div, uint32_t timpre, uint32_t arr)
{
    pclk = apb; hclk = ahb;
    test_rcc.D2CFGR = div; test_rcc.CFGR = timpre;
    htim1.Instance = TIM1;
    htim1.hdma[TIM_DMA_ID_CC3] = &dma;
    htim1.Init.Period = 79; /* Regenerated CubeMX value must be corrected. */
    assert(LED_Strip_Init(80) == HAL_OK);
    assert(htim1.Init.Period == arr && test_tim1.ARR == arr);
    assert(LED_Strip_Show() == HAL_OK);
    htim1.Channel = HAL_TIM_ACTIVE_CHANNEL_3;
    HAL_TIM_PWM_PulseFinishedCallback(&htim1);
}
int main(void)
{
    check_clock(100000000, 200000000, RCC_APB2_DIV2, 0, 249); /* Actual board PLL setup. */
    check_clock(32000000, 64000000, RCC_APB2_DIV2, 0, 79); /* Previous setup. */
    check_clock(50000000, 200000000, RCC_APB2_DIV4, RCC_CFGR_TIMPRE, 249);
    check_clock(25000000, 200000000, 0x600, RCC_CFGR_TIMPRE, 124);
    pclk = 100000001; test_rcc.CFGR = 0; test_rcc.D2CFGR = RCC_APB2_DIV2;
    assert(LED_Strip_Init(80) == HAL_ERROR);
    puts("LED startup tests passed (100 MHz/32 MHz APB2, TIMPRE and DMA startup).");
}
