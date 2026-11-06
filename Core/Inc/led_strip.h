#ifndef LED_STRIP_H
#define LED_STRIP_H

#include "main.h"

#define LED_STRIP_MAX_LEDS 128U

/* Call after MX_TIM1_Init. One task must own this API; do not call from ISRs.
 * Set/Fill/Init return HAL_BUSY during transmission. Show is nonblocking.
 * Brightness is 0..255 (upstream scales by brightness / 256).
 */
HAL_StatusTypeDef LED_Strip_Init(uint16_t count);
HAL_StatusTypeDef LED_Strip_Set(uint16_t index, uint8_t r, uint8_t g,
                               uint8_t b, uint8_t brightness);
HAL_StatusTypeDef LED_Strip_Fill(uint8_t r, uint8_t g, uint8_t b,
                                uint8_t brightness);
HAL_StatusTypeDef LED_Strip_Show(void);
uint8_t LED_Strip_IsBusy(void);

#endif
