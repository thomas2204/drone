#include "gps_raw.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>

#define GPS_DMA_SIZE 2048U

// UART5 DMA cannot use DTCM. D-cache is disabled in the current startup.
static uint8_t gps_dma_rx[GPS_DMA_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));
static SemaphoreHandle_t gps_mutex;
static GPS_RawSnapshot_t gps_data;

void GPS_RawInit(void)
{
    memset(&gps_data, 0, sizeof(gps_data));
    gps_mutex = xSemaphoreCreateMutex();
    configASSERT(gps_mutex != NULL);
}

bool GPS_RawGetSnapshot(GPS_RawSnapshot_t *out)
{
    if (out == NULL || gps_mutex == NULL ||
        xSemaphoreTake(gps_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;

    memcpy(out, &gps_data, sizeof(*out));
    xSemaphoreGive(gps_mutex);
    return true;
}

void GPS_RawTask(void)
{
    uint16_t old_pos = 0;
    uint16_t line_size = 0;
    bool collecting_line = false;
    uint8_t previous_byte = 0;
    char line[GPS_RAW_LINE_SIZE];

    for (;;)
    {
        if (huart5.RxState == HAL_UART_STATE_READY)
        {
            uint32_t error = huart5.ErrorCode;
            if (error != HAL_UART_ERROR_NONE)
            {
                xSemaphoreTake(gps_mutex, portMAX_DELAY);
                gps_data.last_uart_error = error;
                xSemaphoreGive(gps_mutex);
            }

            // Clear startup errors and recover if HAL stopped reception.
            if (HAL_UART_AbortReceive(&huart5) != HAL_OK)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            old_pos = 0;
            line_size = 0;
            collecting_line = false;
            previous_byte = 0;

            if (HAL_UART_Receive_DMA(&huart5, gps_dma_rx,
                                     GPS_DMA_SIZE) != HAL_OK)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            __HAL_DMA_DISABLE_IT(huart5.hdmarx, DMA_IT_HT);
            xSemaphoreTake(gps_mutex, portMAX_DELAY);
            gps_data.starts++;
            xSemaphoreGive(gps_mutex);
        }

        uint16_t pos = (GPS_DMA_SIZE -
            __HAL_DMA_GET_COUNTER(huart5.hdmarx)) % GPS_DMA_SIZE;

        if (pos != old_pos)
        {
            xSemaphoreTake(gps_mutex, portMAX_DELAY);
            gps_data.sample_size = 0;

            while (old_pos != pos)
            {
                uint8_t byte = gps_dma_rx[old_pos];
                old_pos = (old_pos + 1U) % GPS_DMA_SIZE;
                gps_data.rx_bytes++;

                if (gps_data.sample_size < GPS_RAW_SAMPLE_SIZE)
                    gps_data.sample[gps_data.sample_size++] = byte;

                // Only identify candidate headers; no UBX decoding/checksum yet.
                if (previous_byte == 0xB5U && byte == 0x62U)
                    gps_data.ubx_markers++;
                previous_byte = byte;

                // Collect bounded, printable $... lines for raw NMEA inspection.
                // These lines are not yet checksum-validated or decoded.
                if (byte == '$')
                {
                    collecting_line = true;
                    line_size = 0;
                    line[line_size++] = '$';
                }
                else if (collecting_line && byte == '\n')
                {
                    line[line_size] = '\0';
                    memcpy(gps_data.line, line, line_size + 1U);
                    gps_data.nmea_lines++;
                    gps_data.last_line_ms = (uint32_t)(
                        xTaskGetTickCount() * portTICK_PERIOD_MS);
                    collecting_line = false;
                }
                else if (collecting_line && byte != '\r')
                {
                    if (byte >= 32U && byte <= 126U &&
                        line_size < GPS_RAW_LINE_SIZE - 1U)
                        line[line_size++] = (char)byte;
                    else
                        collecting_line = false;
                }
            }

            xSemaphoreGive(gps_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
