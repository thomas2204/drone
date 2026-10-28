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

static void ConfigureReceiver(uint32_t now)
{
    static uint32_t last_attempt_ms;
    bool send = false;
    xSemaphoreTake(gps_mutex, portMAX_DELAY);
    if ((gps_data.config_state == GPS_CONFIG_WAIT && now >= 1000U) ||
        (gps_data.config_state == GPS_CONFIG_SENT &&
         (uint32_t)(now - last_attempt_ms) >= 2000U))
    {
        if (gps_data.config_attempts < 3U)
        {
            gps_data.config_attempts++;
            gps_data.config_state = GPS_CONFIG_SENT;
            last_attempt_ms = now;
            send = true;
        }
        else
            gps_data.config_state = GPS_CONFIG_TIMEOUT;
    }
    xSemaphoreGive(gps_mutex);
    if (send)
    {
        uint8_t packet[48];
        size_t size = GPS_UbxBuild10HzConfig(packet, sizeof(packet));
        // This task is the only UART5 transmitter. DMA reception stays active.
        if (size == 0U || HAL_UART_Transmit(&huart5, packet, (uint16_t)size, 10) != HAL_OK)
        {
            xSemaphoreTake(gps_mutex, portMAX_DELAY);
            gps_data.config_tx_errors++;
            xSemaphoreGive(gps_mutex);
        }
    }
}

// Caller holds gps_mutex. Duplicate epochs must not refresh sample freshness.
static void ReceiveUbx(GPS_UbxParser_t *parser, GPS_UbxEvent_t event, uint32_t now)
{
    if (event == GPS_UBX_PVT)
    {
        if (gps_data.pvt_count != 0U && parser->pvt.tow_ms == gps_data.pvt.tow_ms &&
            parser->pvt.fix_ok == gps_data.pvt.fix_ok &&
            parser->pvt.fix_type == gps_data.pvt.fix_type)
        {
            gps_data.pvt_duplicates++;
            return;
        }
        if (gps_data.pvt_count != 0U)
        {
            uint32_t step = (parser->pvt.tow_ms + GPS_UBX_WEEK_MS -
                             gps_data.pvt.tow_ms) % GPS_UBX_WEEK_MS;
            gps_data.pvt_step_ms = step;
            if (step != 100U) gps_data.pvt_gaps++;
        }
        gps_data.pvt = parser->pvt;
        gps_data.pvt_count++;
        gps_data.last_pvt_ms = now;
    }
    else if (event == GPS_UBX_DOP)
    {
        gps_data.dop = parser->dop;
        gps_data.dop_count++;
        gps_data.last_dop_ms = now;
    }
    else if ((event == GPS_UBX_ACK || event == GPS_UBX_NAK) &&
             parser->acknowledged_class == 6U && parser->acknowledged_id == 0x8AU)
    {
        if (event == GPS_UBX_ACK)
        {
            gps_data.config_acks++;
            gps_data.config_state = GPS_CONFIG_ACKED;
        }
        else
        {
            gps_data.config_naks++;
            gps_data.config_state = GPS_CONFIG_REJECTED;
        }
    }
    else if (event == GPS_UBX_BAD_FRAME || event == GPS_UBX_BAD_PVT || event == GPS_UBX_BAD_DOP)
        gps_data.ubx_errors++;
}

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
    uint32_t last_rx_ms = 0;
    static GPS_UbxParser_t ubx;
    GPS_UbxReset(&ubx);
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
            GPS_UbxReset(&ubx);

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
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (GPS_UbxBusy(&ubx) && (uint32_t)(now - last_rx_ms) > 250U)
            GPS_UbxReset(&ubx);

        if (pos != old_pos)
        {
            last_rx_ms = now;
            xSemaphoreTake(gps_mutex, portMAX_DELAY);
            gps_data.sample_size = 0;

            while (old_pos != pos)
            {
                uint8_t byte = gps_dma_rx[old_pos];
                old_pos = (old_pos + 1U) % GPS_DMA_SIZE;
                gps_data.rx_bytes++;

                if (gps_data.sample_size < GPS_RAW_SAMPLE_SIZE)
                    gps_data.sample[gps_data.sample_size++] = byte;

                if (previous_byte == 0xB5U && byte == 0x62U)
                    gps_data.ubx_markers++;
                previous_byte = byte;

                bool binary = GPS_UbxBusy(&ubx) || byte == 0xB5U;
                GPS_UbxEvent_t event = GPS_UbxFeed(&ubx, byte);
                ReceiveUbx(&ubx, event, now);
                if (binary)
                {
                    // Binary payload can contain '$' and newline bytes.
                    collecting_line = false;
                    continue;
                }

                // Keep raw text, then checksum-validate and decode GGA fixes.
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
                    // Keep diagnostic copies independently of decoder acceptance.
                    // Match any talker prefix (GN, GP, GA, etc.).
                    if (line_size >= 7U && memcmp(line + 3, "GSA,", 4) == 0)
                    {
                        memcpy(gps_data.gsa_line, line, line_size + 1U);
                        gps_data.gsa_count++;
                        GPS_NmeaResult_t result = GPS_NmeaParseGsa(line, &gps_data.gsa);
                        if (result == GPS_NMEA_GSA)
                        {
                            gps_data.gsa_decoded_count++;
                            gps_data.last_gsa_ms = gps_data.last_line_ms;
                        }
                        else
                            gps_data.gsa_errors++;
                    }
                    else if (line_size >= 7U && memcmp(line + 3, "RMC,", 4) == 0)
                    {
                        memcpy(gps_data.rmc_line, line, line_size + 1U);
                        gps_data.rmc_count++;
                        GPS_NmeaResult_t result = GPS_NmeaParseRmc(line, &gps_data.rmc);
                        if (result == GPS_NMEA_RMC)
                        {
                            gps_data.rmc_decoded_count++;
                            gps_data.last_rmc_ms = gps_data.last_line_ms;
                        }
                        else
                            gps_data.rmc_errors++;
                    }
                    GPS_NmeaResult_t result = GPS_NmeaParseGga(line, &gps_data.fix);
                    if (result == GPS_NMEA_GGA)
                    {
                        gps_data.gga_count++;
                        gps_data.last_gga_ms = gps_data.last_line_ms;
                    }
                    else if (result != GPS_NMEA_IGNORED)
                        gps_data.gga_errors++;
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

        ConfigureReceiver(now);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
