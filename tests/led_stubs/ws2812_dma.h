#include "led_strip.h"
typedef struct {
    TIM_HandleTypeDef *htim;
    uint32_t channel;
    uint16_t *pwmBuffer;
    uint16_t ledCount, totalSize;
} WS2812_Strand;
void WS2812_SetPixel(WS2812_Strand *, uint16_t, uint8_t, uint8_t, uint8_t, uint8_t);
