#ifndef LED_TEST_HAL_H
#define LED_TEST_HAL_H
#include <stddef.h>
#include <stdint.h>
#define LED_STRIP_MAX_LEDS 128U
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY } HAL_StatusTypeDef;
typedef struct { uint32_t ARR, EGR; } TIM_TypeDef;
typedef struct { struct { uint32_t Mode; } Init; } DMA_HandleTypeDef;
typedef struct {
    TIM_TypeDef *Instance;
    struct { uint32_t Prescaler, Period; } Init;
    DMA_HandleTypeDef *hdma[4];
    uint32_t Channel;
} TIM_HandleTypeDef;
typedef struct { uint32_t D2CFGR, CFGR; } Test_RCC;
typedef struct { uint32_t CCR; } Test_SCB;
extern Test_RCC test_rcc;
extern Test_SCB test_scb;
extern TIM_TypeDef test_tim1;
extern TIM_HandleTypeDef htim1;
#define RCC (&test_rcc)
#define SCB (&test_scb)
#define TIM1 (&test_tim1)
#define RCC_D2CFGR_D2PPRE2 0x700U
#define RCC_APB2_DIV1 0U
#define RCC_APB2_DIV2 0x400U
#define RCC_APB2_DIV4 0x500U
#define RCC_CFGR_TIMPRE 0x8000U
#define TIM_DMA_ID_CC3 3
#define DMA_NORMAL 0
#define TIM_CHANNEL_3 8
#define HAL_TIM_ACTIVE_CHANNEL_3 4
#define TIM_EGR_UG 1
#define SCB_CCR_DC_Msk 0x10000U
#define __HAL_TIM_SET_AUTORELOAD(t,v) ((t)->Instance->ARR = (v))
#define __HAL_TIM_SET_COUNTER(t,v) ((void)(t),(void)(v))
#define __HAL_TIM_SET_COMPARE(t,c,v) ((void)(t),(void)(c),(void)(v))
#define __DSB() ((void)0)
uint32_t HAL_RCC_GetPCLK2Freq(void);
uint32_t HAL_RCC_GetHCLKFreq(void);
void SCB_CleanDCache_by_Addr(uint32_t *p, int32_t n);
HAL_StatusTypeDef HAL_TIM_PWM_Start_DMA(TIM_HandleTypeDef *, uint32_t, uint32_t *, uint16_t);
HAL_StatusTypeDef HAL_TIM_PWM_Stop_DMA(TIM_HandleTypeDef *, uint32_t);
HAL_StatusTypeDef LED_Strip_Init(uint16_t);
HAL_StatusTypeDef LED_Strip_Fill(uint8_t, uint8_t, uint8_t, uint8_t);
HAL_StatusTypeDef LED_Strip_Show(void);
#endif
