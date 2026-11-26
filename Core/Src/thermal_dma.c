#include "main.h"
#include "i2c.h"
#include "cmsis_os2.h"
#include "thermal_dma.h"
extern I2C_HandleTypeDef hi2c1;
DMA_HandleTypeDef hdma_i2c1_rx;
osSemaphoreId_t i2c_done;

void MLX_I2C_DmaInit(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();
    hdma_i2c1_rx.Instance                 = DMA1_Stream6;
    hdma_i2c1_rx.Init.Request             = DMA_REQUEST_I2C1_RX;
    hdma_i2c1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_i2c1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_i2c1_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_i2c1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_i2c1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_i2c1_rx.Init.Mode                = DMA_NORMAL;
    hdma_i2c1_rx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_i2c1_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_i2c1_rx) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&hi2c1, hdmarx, hdma_i2c1_rx);

    HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 5, 0);  HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);
    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 5, 0);       HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, 5, 0);       HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
}