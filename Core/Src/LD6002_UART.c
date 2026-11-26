#include "ld6002.h"
#include "main.h"
#include "cmsis_os2.h"
#include "usart.h"
#include "ff.h"
#include "app_storage.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

extern UART_HandleTypeDef huart3;

#define LD_RX_SZ 1024
__attribute__((section(".dma_buffer"), aligned(32)))
static uint8_t ld_rx[LD_RX_SZ];     /* must NOT be in DTCM: DMA can't reach it */
static uint32_t ld_tail;
static FIL fil2 __attribute__((section(".sd_dma_buffer"), aligned(32)));

DMA_HandleTypeDef hdma_usart3_rx;

static void LD_UART_DmaInit(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_usart3_rx.Instance                 = DMA1_Stream7;
    hdma_usart3_rx.Init.Request             = DMA_REQUEST_USART3_RX;
    hdma_usart3_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_usart3_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart3_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode                = DMA_CIRCULAR;
    hdma_usart3_rx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_usart3_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart3_rx) != HAL_OK) Error_Handler();

    __HAL_LINKDMA(&huart3, hdmarx, hdma_usart3_rx);

    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_TC_FLAG_INDEX(&hdma_usart3_rx));
    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_HT_FLAG_INDEX(&hdma_usart3_rx));
    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_TE_FLAG_INDEX(&hdma_usart3_rx));
    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_FE_FLAG_INDEX(&hdma_usart3_rx));
    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_DME_FLAG_INDEX(&hdma_usart3_rx));

    HAL_NVIC_SetPriority(DMA1_Stream7_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream7_IRQn);
    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
}

void LD6002_Start(void)
{
    LD_UART_DmaInit();            /* must come before Receive_DMA */
    ld6002_init();
    ld_tail = 0;
    if (HAL_UART_Receive_DMA(&huart3, ld_rx, LD_RX_SZ) != HAL_OK) {
        Error_Handler();
    }
    //USART3->CR3 |= USART_CR3_DMAR;
    static const uint8_t q[] = {0x01,0x00,0x01,0x00,0x00,0xFF,0xFF,0xFF};
    HAL_UART_Transmit(&huart3, q, sizeof q, 100);
    
}

void LD6002_RxRestart(void);
/* Called periodically from a task: drain whatever DMA has written. */
void LD6002_Poll(void)
{
    if (huart3.RxState == HAL_UART_STATE_READY) {
        LD6002_RxRestart();
        return;
    }
    uint32_t head = LD_RX_SZ - __HAL_DMA_GET_COUNTER(huart3.hdmarx);
    while (ld_tail != head) {
        ld6002_feed(ld_rx[ld_tail], HAL_GetTick());
        ld_tail = (ld_tail + 1) % LD_RX_SZ;
    }

}

/* If an overrun or framing error stops reception, restart it. */
volatile uint32_t ld_restarts;
void LD6002_RxRestart(void)
{
    ld_restarts++;
    ld_tail = 0;

    // 1. Abort current reception
    //(void)HAL_UART_AbortReceive(&huart3);

    // 2. Force clear all UART error flags (especially ORE)
    //__HAL_UART_CLEAR_FLAG(&huart3, UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_PEF);

    // 3. Clear DMA error flags
    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_TE_FLAG_INDEX(&hdma_usart3_rx));
    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_DME_FLAG_INDEX(&hdma_usart3_rx));
    //__HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, __HAL_DMA_GET_FE_FLAG_INDEX(&hdma_usart3_rx));

    // 4. Restart DMA
    if (HAL_UART_Receive_DMA(&huart3, ld_rx, LD_RX_SZ) != HAL_OK) {
        // Error handling
    }
    //USART3->CR3 |= USART_CR3_DMAR;
}

static int x100(float v) { return (int)(v * 100.0f + (v < 0.0f ? -0.5f : 0.5f)); }

static void log_frame(uint32_t now, const ld6002_data_t *d)
{
    if (AppStorage_Open(&fil2, "LD6002.CSV", FA_OPEN_APPEND | FA_WRITE) != FR_OK) return;
    char line[128];
    int n = snprintf(line, sizeof line, "%lu,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%lu\r\n",
        (unsigned long)now, d->present,
        x100(d->breath_bpm), x100(d->heart_bpm),
        x100(d->phase_total), x100(d->phase_breath), x100(d->phase_heart),
        x100(d->range_cm), x100(d->x),
        (unsigned long)d->frames_ok, (unsigned long)d->frames_bad);
    UINT bw;
    f_write(&fil2, line, (UINT)n, &bw);
    AppStorage_Close(&fil2);
}

void LD6002_Task(void *arg)
{
    LD6002_Start();
    uint32_t last_log = 0;
    for (;;) {
        LD6002_Poll();
        uint32_t now = HAL_GetTick();
        if (now - last_log >= 1000) {
            last_log = now;
            log_frame(now, ld6002_get());
        }
        osDelay(5);
    }
}