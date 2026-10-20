/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define UWB_IRQ_Pin GPIO_PIN_6
#define UWB_IRQ_GPIO_Port GPIOF
#define UWB_IRQ_EXTI_IRQn EXTI9_5_IRQn
#define UWB_CS_Pin GPIO_PIN_10
#define UWB_CS_GPIO_Port GPIOF
#define FLOW_IRQ_Pin GPIO_PIN_5
#define FLOW_IRQ_GPIO_Port GPIOC
#define FLOW_IRQ_EXTI_IRQn EXTI9_5_IRQn
#define BAT_VOLTAGE_Pin GPIO_PIN_1
#define BAT_VOLTAGE_GPIO_Port GPIOB
#define CAM_RESET_N_Pin GPIO_PIN_6
#define CAM_RESET_N_GPIO_Port GPIOG
#define CAM_PWDN_Pin GPIO_PIN_7
#define CAM_PWDN_GPIO_Port GPIOG
#define SD_CARD_DETECT_Pin GPIO_PIN_7
#define SD_CARD_DETECT_GPIO_Port GPIOC
#define WS2812_DATA_Pin GPIO_PIN_10
#define WS2812_DATA_GPIO_Port GPIOA
#define FLOW_CS_Pin GPIO_PIN_13
#define FLOW_CS_GPIO_Port GPIOG
#define AUDIO_SPK_BCLK_Pin GPIO_PIN_5
#define AUDIO_SPK_BCLK_GPIO_Port GPIOI
#define AUDIO_SPK_DATA_Pin GPIO_PIN_6
#define AUDIO_SPK_DATA_GPIO_Port GPIOI
#define AUDIO_SPK_LRCLK_Pin GPIO_PIN_7
#define AUDIO_SPK_LRCLK_GPIO_Port GPIOI

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
