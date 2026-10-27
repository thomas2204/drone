
#include "mk_app.h"

#include "main.h"
#include "usart.h"
#include "dma.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "ardupilotmega/mavlink.h"
#include "gps_input.h"

#include <string.h>

// =====================================
// Configuration
// =====================================

#define MAV_UART           huart1
#define MAV_RX_DMA_SIZE 1024


// =====================================
// DMA buffer
// =====================================

// 32-byte aligned for STM32H7 DCache.
// MUST be placed in DMA-accessible SRAM,
// not DTCM.
//
// Make sure your linker places this buffer
// in AXI SRAM or D2 SRAM.


static uint8_t dma_rx[MAV_RX_DMA_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32))); //store raw mavelink readings

static uint16_t old_pos = 0;  //Tracks the next buffer position that your task needs to read.

// =====================================
// Shared data
// =====================================

static FC_Telemetry_t fc_data;   //edcoded data
static SemaphoreHandle_t fc_mutex = NULL;  //mutex: a lock protecting fc_data while one task updates it and another copies it.

static volatile uint32_t rx_byte_count = 0;
static uint8_t fc_system_id = 1;

// =====================================
// Initialization
// =====================================

void MAVLink_AppInit(void)
{
    memset(&fc_data, 0, sizeof(fc_data));  //Clears the telemetry structure

    fc_mutex = xSemaphoreCreateMutex();  //Creates the lock and checks that creation succeeded.

    configASSERT(fc_mutex != NULL);
}

// =====================================
// Decode MAVLink messages
// =====================================

static void ProcessMessage(
    const mavlink_message_t *msg)
{
    // Accept only autopilot component
    if (msg->compid != MAV_COMP_ID_AUTOPILOT1)
        return;

    if (xSemaphoreTake(
        fc_mutex, portMAX_DELAY) != pdTRUE)
        return;

    uint32_t now =
        (uint32_t)(xTaskGetTickCount()
                   * portTICK_PERIOD_MS);

    switch (msg->msgid)  //hooses what to do based on message type.
    {
        case MAVLINK_MSG_ID_HEARTBEAT:
        {
            mavlink_heartbeat_t hb;

            mavlink_msg_heartbeat_decode(
                msg, &hb);

            if (hb.autopilot !=
                MAV_AUTOPILOT_ARDUPILOTMEGA)
                break;

            fc_data.heartbeat_count++;
            fc_system_id = msg->sysid;

            fc_data.last_heartbeat_ms = now;

            fc_data.armed =
                (hb.base_mode &
                MAV_MODE_FLAG_SAFETY_ARMED) != 0;

            fc_data.flight_mode =
                hb.custom_mode;

            break;
        }

        case MAVLINK_MSG_ID_ATTITUDE:
        {
            mavlink_attitude_t a;

            mavlink_msg_attitude_decode(
                msg, &a);

            fc_data.attitude_count++;
            fc_data.last_attitude_ms = now;

            fc_data.roll = a.roll;
            fc_data.pitch = a.pitch;
            fc_data.yaw = a.yaw;

            fc_data.roll_rate = a.rollspeed;
            fc_data.pitch_rate = a.pitchspeed;
            fc_data.yaw_rate = a.yawspeed;

            break;
        }
        default:
            break;
    }

    xSemaphoreGive(fc_mutex); //unlock
}

// =====================================
// Main MAVLink Task
// =====================================

// Called only by MAVLink_Task: USART1 has one transmitter and MAVLink sequence owner.
static void ForwardGps(uint32_t now)
{
    static uint32_t last_poll_ms;
    static uint32_t last_tx_ms;
    static uint32_t sent_rmc_count;
    static GPS_RawSnapshot_t gps;
    if ((uint32_t)(now - last_poll_ms) < 20U ||
        (uint32_t)(now - last_tx_ms) < 200U) return;
    last_poll_ms = now;
    if (!GPS_RawGetSnapshot(&gps) || gps.rmc_decoded_count == sent_rmc_count)
        return;
    mavlink_gps_input_t input;
    if (!GPS_BuildInput(&gps, now, &input)) return;
    mavlink_message_t message;
    uint8_t packet[MAVLINK_MAX_PACKET_LEN];
    mavlink_get_channel_status(MAVLINK_COMM_0)->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_msg_gps_input_encode_chan(fc_system_id, MAV_COMP_ID_ONBOARD_COMPUTER,
                                    MAVLINK_COMM_0, &message, &input);
    uint16_t length = mavlink_msg_to_send_buffer(packet, &message);
    // 77 bytes take under 2ms at 460800 baud. RX DMA continues during polling TX.
    HAL_StatusTypeDef result = HAL_UART_Transmit(&MAV_UART, packet, length, 5);
    last_tx_ms = now;
    if (result == HAL_OK) sent_rmc_count = gps.rmc_decoded_count;
    if (xSemaphoreTake(fc_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (result == HAL_OK)
        {
            fc_data.gps_tx_count++;
            fc_data.gps_tx_fix_type = input.fix_type;
        }
        else
            fc_data.gps_tx_errors++;
        xSemaphoreGive(fc_mutex);
    }
}

void MAVLink_Task(void)
{
    mavlink_message_t msg;
    mavlink_status_t status;

    memset(&msg, 0, sizeof(msg));
    memset(&status, 0, sizeof(status));
    /*
    // Clean cache state before starting DMA
    SCB_InvalidateDCache_by_Addr(
        (uint32_t *)dma_rx,
        MAV_RX_DMA_SIZE
    );
    */

    if (HAL_UART_Receive_DMA(
        &MAV_UART,
        dma_rx,
        MAV_RX_DMA_SIZE) != HAL_OK)
    {
        Error_Handler();
    }

    // Poll the circular DMA pointer; disable half-transfer interrupts.
    __HAL_DMA_DISABLE_IT(
        MAV_UART.hdmarx,
        DMA_IT_HT
    );

    old_pos = 0;

    for (;;)
    {
        // HAL stops DMA reception after UART framing/overrun errors.
        // Recover here in task context once HAL marks reception ready.
        if (MAV_UART.RxState == HAL_UART_STATE_READY)
        {
            // AbortReceive also clears error flags and flushes pending RX data.
            if (HAL_UART_AbortReceive(&MAV_UART) != HAL_OK)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            old_pos = 0;
            mavlink_reset_channel_status(MAVLINK_COMM_0);

            if (HAL_UART_Receive_DMA(
                    &MAV_UART,
                    dma_rx,
                    MAV_RX_DMA_SIZE) != HAL_OK)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            __HAL_DMA_DISABLE_IT(MAV_UART.hdmarx, DMA_IT_HT);

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // DMA remaining transfer counter
        uint16_t remaining =
            __HAL_DMA_GET_COUNTER(
                MAV_UART.hdmarx);

        uint16_t pos =
            (MAV_RX_DMA_SIZE - remaining)
            % MAV_RX_DMA_SIZE;

        if (pos != old_pos)
        {
            /*
            // DMA may have updated cacheable SRAM
            SCB_InvalidateDCache_by_Addr(
                (uint32_t *)dma_rx,
                MAV_RX_DMA_SIZE
            );
            */
            // Process all newly received bytes
            while (old_pos != pos)
            {
                uint8_t byte = dma_rx[old_pos];

                old_pos++;

                if (old_pos >= MAV_RX_DMA_SIZE)
                    old_pos = 0;

                rx_byte_count++;

                if (mavlink_parse_char(
                    MAVLINK_COMM_0,
                    byte,
                    &msg,
                    &status))
                {
                    ProcessMessage(&msg);
                }
            }
        }

        ForwardGps((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));

        // Poll once per millisecond
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// =====================================
// Thread-safe public telemetry access
// =====================================

bool MAVLink_GetTelemetry(
    FC_Telemetry_t *out)
{
    if (out == NULL || fc_mutex == NULL)
        return false;

    if (xSemaphoreTake(
        fc_mutex,
        pdMS_TO_TICKS(20)) != pdTRUE)
        return false;

    memcpy(out, &fc_data, sizeof(*out));

    xSemaphoreGive(fc_mutex);

    uint32_t now =
        (uint32_t)(xTaskGetTickCount()
                   * portTICK_PERIOD_MS);

    out->connected =
        (out->last_heartbeat_ms != 0) &&
        ((uint32_t)(now -
          out->last_heartbeat_ms) < 3000U);

    return true;
}

uint32_t MAVLink_GetRxByteCount(void)
{
    return rx_byte_count;  //dignostic count for byte recived 
}
