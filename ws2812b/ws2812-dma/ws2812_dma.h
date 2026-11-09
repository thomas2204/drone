// Copyright (c) 2026 Simonbln. Apache 2.0 license. See LICENSE file.

#ifndef WS2812_DMA_H
#define WS2812_DMA_H

#include "main.h"
#include <stdint.h>

typedef enum {
    COLOR_RED,
    COLOR_ORANGE,
    COLOR_YELLOW,
    COLOR_GREEN,
    COLOR_CYAN,
    COLOR_BLUE,
    COLOR_MAGENTA,
    COLOR_WHITE,
    COLOR_OFF
} WS2812_Color_t;

#define WS2812_RESET_PULSE 60

typedef struct {
    TIM_HandleTypeDef* htim;
    uint32_t channel;
    uint16_t* pwmBuffer;
    uint16_t ledCount;
    uint16_t totalSize; // (24 * ledCount) + reset_pulses
} WS2812_Strand;

void WS2812_SetPixel(WS2812_Strand* strand, uint16_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
void WS2812_SetColor(WS2812_Strand* strand, uint16_t index, WS2812_Color_t color, uint8_t brightness);
void WS2812_StartChain(WS2812_Strand** strandArray, uint8_t numStrands);
void WS2812_HandleCallback(TIM_HandleTypeDef* htim);

#endif
