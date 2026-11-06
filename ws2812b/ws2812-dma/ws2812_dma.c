// Copyright (c) 2026 Simonbln. Apache 2.0 license. See LICENSE file.

#include "ws2812_dma.h"

static WS2812_Strand** _strands = NULL;
static uint8_t _numStrands = 0;
static volatile uint8_t _currentIdx = 0;
static volatile uint8_t _isBusy = 0;

void WS2812_SetPixel(WS2812_Strand* strand, uint16_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    if (index >= strand->ledCount) return;

    uint8_t rs = (uint16_t)(r * brightness) >> 8;
    uint8_t gs = (uint16_t)(g * brightness) >> 8;
    uint8_t bs = (uint16_t)(b * brightness) >> 8;

    uint32_t color = (gs << 16) | (rs << 8) | bs;
    
	uint16_t period = strand->htim->Instance->ARR + 1;

	uint16_t high = (period * 2) / 3;
	uint16_t low  = (period * 1) / 3;

    for (int i = 23; i >= 0; i--) {
        strand->pwmBuffer[index * 24 + (23 - i)] = (color & (1UL << i)) ? high : low;
    }
}

void WS2812_SetColor(WS2812_Strand* strand, uint16_t index, WS2812_Color_t color, uint8_t brightness) {
    uint8_t r = 0, g = 0, b = 0;
    switch (color) {
        case COLOR_RED:     r = 255; break;
        case COLOR_ORANGE:  r = 255; g = 80;  break;
        case COLOR_YELLOW:  r = 255; g = 180; break;
        case COLOR_GREEN:   g = 255; break;
        case COLOR_CYAN:    g = 255; b = 255; break;
        case COLOR_BLUE:    b = 255; break;
        case COLOR_MAGENTA: r = 255; b = 255; break;
        case COLOR_WHITE:   r = 255; g = 255; b = 255; break;
        case COLOR_OFF:     default: break;
    }
    WS2812_SetPixel(strand, index, r, g, b, brightness);
}

void WS2812_StartChain(WS2812_Strand** strandArray, uint8_t numStrands) {
    if (_isBusy || numStrands == 0 || strandArray == NULL) return;

    _strands = strandArray;
    _numStrands = numStrands;
    _currentIdx = 0;
    _isBusy = 1;

    HAL_TIM_PWM_Start_DMA(_strands[0]->htim, _strands[0]->channel, 
                         (uint32_t*)_strands[0]->pwmBuffer, _strands[0]->totalSize);
}

void WS2812_HandleCallback(TIM_HandleTypeDef* htim) {
    if (!_isBusy || _strands == NULL) return;

    if (htim->Instance == _strands[_currentIdx]->htim->Instance) {

        HAL_TIM_PWM_Stop_DMA(_strands[_currentIdx]->htim, _strands[_currentIdx]->channel);
        
        _currentIdx++;

        if (_currentIdx < _numStrands) {
            HAL_TIM_PWM_Start_DMA(_strands[_currentIdx]->htim, _strands[_currentIdx]->channel, 
                                 (uint32_t*)_strands[_currentIdx]->pwmBuffer, _strands[_currentIdx]->totalSize);
        } else {
            _isBusy = 0;
        }
    }
}
