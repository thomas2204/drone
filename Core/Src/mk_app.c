
#include "mk_app.h"

#include "main.h"
#include "usart.h"
#include "dma.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "ardupilotmega/mavlink.h"
#include "gps_input.h"
#if CAMERA_FLOW_BENCH
#include "camera_flow.h"
#endif
#include "patrol.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

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
static Patrol patrol;
static PatrolInput patrol_input;
static volatile unsigned patrol_request;
static uint32_t patrol_position_ms, patrol_ekf_ms, patrol_range_ms, patrol_ground_ms;
static bool patrol_range_valid, patrol_ekf_valid;
static uint32_t patrol_range_boot_ms;
static uint8_t patrol_range_id;
static bool patrol_range_seen;
static float patrol_slant_m;
static uint8_t patrol_system_status;

#if CAMERA_FLOW_MAVLINK
static struct {
    uint32_t tx_count, rx_count, received_ms;
    float rate_x, rate_y;
    uint8_t quality;
} flow_link;
#endif

// =====================================
// Initialization
// =====================================

void MAVLink_AppInit(void)
{
    memset(&fc_data, 0, sizeof(fc_data));  //Clears the telemetry structure
    Patrol_Init(&patrol);

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

    if (msg->msgid != MAVLINK_MSG_ID_HEARTBEAT &&
        (fc_data.heartbeat_count == 0U || msg->sysid != fc_system_id)) {
        xSemaphoreGive(fc_mutex); return;
    }

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
            if (fc_system_id != msg->sysid)
                fc_data.sys_status_count = 0U;
            fc_system_id = msg->sysid;

            fc_data.last_heartbeat_ms = now;

            fc_data.armed =
                (hb.base_mode &
                MAV_MODE_FLAG_SAFETY_ARMED) != 0;

            fc_data.flight_mode =
                hb.custom_mode;
            patrol_system_status = hb.system_status;

            break;
        }

        case MAVLINK_MSG_ID_SYS_STATUS:
        {
            if (fc_data.heartbeat_count == 0U || msg->sysid != fc_system_id) break;
            mavlink_sys_status_t state;
            mavlink_msg_sys_status_decode(msg, &state);
            const uint32_t bit = MAV_SYS_STATUS_PREARM_CHECK;
            fc_data.prearm_supported = (state.onboard_control_sensors_present & bit) != 0U;
            fc_data.prearm_enabled = (state.onboard_control_sensors_enabled & bit) != 0U;
            fc_data.prearm_healthy = (state.onboard_control_sensors_health & bit) != 0U;
            fc_data.sys_status_count++;
            fc_data.last_sys_status_ms = now;
            uint32_t enabled = state.onboard_control_sensors_enabled &
                               state.onboard_control_sensors_present;
            /* Pre-arm is checked separately: its health may change when armed. */
            enabled &= ~MAV_SYS_STATUS_PREARM_CHECK;
            patrol_input.healthy = enabled != 0U &&
                (enabled & ~state.onboard_control_sensors_health) == 0U;
            break;
        }

        case MAVLINK_MSG_ID_LOCAL_POSITION_NED: {
            mavlink_local_position_ned_t pos;
            mavlink_msg_local_position_ned_decode(msg, &pos);
            patrol_input.x=pos.x; patrol_input.y=pos.y; patrol_input.z=pos.z;
            patrol_input.vx=pos.vx; patrol_input.vy=pos.vy; patrol_input.vz=pos.vz;
            patrol_position_ms=now;
            break;
        }
        case MAVLINK_MSG_ID_EKF_STATUS_REPORT: {
            mavlink_ekf_status_report_t ekf;
            mavlink_msg_ekf_status_report_decode(msg, &ekf);
            const uint16_t needed = EKF_ATTITUDE | EKF_VELOCITY_HORIZ |
                EKF_VELOCITY_VERT | EKF_POS_HORIZ_ABS | EKF_POS_VERT_ABS;
            patrol_ekf_valid = (ekf.flags & needed) == needed &&
                !(ekf.flags & (EKF_UNINITIALIZED | EKF_CONST_POS_MODE | EKF_GPS_GLITCHING));
            patrol_ekf_ms=now;
            break;
        }
        case MAVLINK_MSG_ID_DISTANCE_SENSOR: {
            mavlink_distance_sensor_t range;
            mavlink_msg_distance_sensor_decode(msg, &range);
            if (range.orientation != MAV_SENSOR_ROTATION_PITCH_270 ||
                range.type != MAV_DISTANCE_SENSOR_LASER) break;
            /* Lock to one downward laser; never mix multiple rangefinders.
             * Duplicate sensor epochs must not refresh freshness. */
            if (patrol_range_seen && range.id != patrol_range_id) break;
            if (patrol_range_seen && range.time_boot_ms == patrol_range_boot_ms) break;
            patrol_range_seen=true; patrol_range_id=range.id;
            patrol_range_boot_ms=range.time_boot_ms;
            patrol_range_ms=now;
            patrol_range_valid = range.current_distance >= range.min_distance &&
                range.current_distance < range.max_distance &&
                range.current_distance > 0U && range.max_distance >= 400U &&
                range.signal_quality != 1U;
            patrol_slant_m=range.current_distance*0.01f;
            break;
        }
        case MAVLINK_MSG_ID_EXTENDED_SYS_STATE: {
            mavlink_extended_sys_state_t state;
            mavlink_msg_extended_sys_state_decode(msg, &state);
            patrol_input.on_ground=state.landed_state == MAV_LANDED_STATE_ON_GROUND;
            patrol_ground_ms=now;
            break;
        }
#if CAMERA_FLOW_MAVLINK
        case MAVLINK_MSG_ID_OPTICAL_FLOW: {
            mavlink_optical_flow_t report;
            mavlink_msg_optical_flow_decode(msg, &report);
            if (!isfinite(report.flow_rate_x) || !isfinite(report.flow_rate_y)) break;
            ++flow_link.rx_count;
            flow_link.received_ms = now;
            flow_link.rate_x = report.flow_rate_x;
            flow_link.rate_y = report.flow_rate_y;
            flow_link.quality = report.quality;
            break;
        }
#endif
        case MAVLINK_MSG_ID_COMMAND_ACK: {
            mavlink_command_ack_t ack;
            mavlink_msg_command_ack_decode(msg, &ack);
            if (ack.command == MAV_CMD_COMPONENT_ARM_DISARM || ack.command == MAV_CMD_NAV_TAKEOFF) {
                patrol_input.ack_command=ack.command;
                patrol_input.ack_result=ack.result;
                patrol_input.ack_serial++;
            }
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
    static uint32_t sent_rmc_count;
    static uint32_t sent_pvt_count;
    static GPS_RawSnapshot_t gps;
    if ((uint32_t)(now - last_poll_ms) < 5U) return;
    last_poll_ms = now;
    if (!GPS_RawGetSnapshot(&gps)) return;
    bool ubx = gps.pvt_count != 0U;
    if (ubx ? gps.pvt_count == sent_pvt_count : gps.rmc_decoded_count == sent_rmc_count) return;
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
    // Forward each new epoch once. Rate is set at the receiver (100 ms),
    // rather than retransmitting stale data or losing epochs to timer jitter.
    if (result == HAL_OK)
    {
        if (ubx) sent_pvt_count = gps.pvt_count;
        else sent_rmc_count = gps.rmc_decoded_count;
    }
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

/* Same task/transmitter as GPS forwarding. Ask for 1 Hz SYS_STATUS only
 * when it is missing or stale; do not issue arming or pre-arm commands. */
#if CAMERA_FLOW_BENCH
static int16_t ScaleImu(float value)
{
    return (int16_t)fmaxf(-32767.0f, fminf(32767.0f, value * 1000.0f));
}

void MAVLink_GetFlowReference(CameraFlowReference *reference)
{
    *reference = (CameraFlowReference){.imu_age_ms = UINT16_MAX, .tof_age_ms = UINT16_MAX};
#if CAMERA_FLOW_MAVLINK
    reference->fc_flow_age_ms = UINT16_MAX;
#endif
    if (!fc_mutex || xSemaphoreTake(fc_mutex, 0) != pdTRUE) return;
    const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    const uint32_t imu_age = now - fc_data.last_attitude_ms;
    const uint32_t tof_age = now - patrol_range_ms;
    if (fc_data.attitude_count && imu_age < UINT16_MAX &&
        isfinite(fc_data.roll) && isfinite(fc_data.pitch) && isfinite(fc_data.yaw) &&
        isfinite(fc_data.roll_rate) && isfinite(fc_data.pitch_rate) && isfinite(fc_data.yaw_rate)) {
        reference->imu_age_ms = (uint16_t)imu_age;
        reference->roll_mrad = ScaleImu(fc_data.roll);
        reference->pitch_mrad = ScaleImu(fc_data.pitch);
        reference->yaw_mrad = ScaleImu(fc_data.yaw);
        reference->gx_mrad_s = ScaleImu(fc_data.roll_rate);
        reference->gy_mrad_s = ScaleImu(fc_data.pitch_rate);
        reference->gz_mrad_s = ScaleImu(fc_data.yaw_rate);
    }
    if (patrol_range_valid && patrol_range_seen && tof_age < UINT16_MAX &&
        patrol_slant_m > 0.0f && patrol_slant_m < 65.535f) {
        reference->tof_age_ms = (uint16_t)tof_age;
        reference->tof_mm = (uint16_t)(patrol_slant_m * 1000.0f + 0.5f);
    }
#if CAMERA_FLOW_MAVLINK
    reference->flow_tx_count = flow_link.tx_count;
    reference->fc_flow_rx_count = flow_link.rx_count;
    const uint32_t flow_age = now - flow_link.received_ms;
    if (flow_link.rx_count && flow_age < UINT16_MAX) {
        reference->fc_flow_age_ms = (uint16_t)flow_age;
        reference->fc_flow_x_mrad_s = ScaleImu(flow_link.rate_x);
        reference->fc_flow_y_mrad_s = ScaleImu(flow_link.rate_y);
        reference->fc_flow_quality = flow_link.quality;
    }
#endif
    xSemaphoreGive(fc_mutex);
}
#endif

#if CAMERA_FLOW_MAVLINK
static void ForwardCameraFlow(uint32_t now)
{
    static uint32_t last_sequence;
    static uint32_t last_report_request_ms;
    static bool requested_report;
    CameraFlowReading sample;
    if (!fc_data.heartbeat_count || fc_data.armed || now - fc_data.last_heartbeat_ms >= 2000U ||
        !CameraFlow_Read(&sample) || sample.sequence == last_sequence ||
        !patrol_range_valid || now-patrol_range_ms >= 200U ||
        !fc_data.attitude_count || now-fc_data.last_attitude_ms >= 150U) return;
    mavlink_message_t message;
    uint8_t wire[MAVLINK_MAX_PACKET_LEN];
    mavlink_get_channel_status(MAVLINK_COMM_0)->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_msg_optical_flow_pack_chan(fc_system_id, MAV_COMP_ID_ONBOARD_COMPUTER,
        MAVLINK_COMM_0, &message, (uint64_t)sample.capture_ms * 1000U,
        0, 0, 0, 0.0f, 0.0f, sample.quality, -1.0f, sample.rate_x, sample.rate_y);
    uint16_t length = mavlink_msg_to_send_buffer(wire, &message);
    if (HAL_UART_Transmit(&MAV_UART, wire, length, 5) != HAL_OK) return;
    last_sequence = sample.sequence;
    xSemaphoreTake(fc_mutex, portMAX_DELAY);
    ++flow_link.tx_count;
    xSemaphoreGive(fc_mutex);

    /* Request the FC's own processed flow report on this link while testing. */
    if ((flow_link.rx_count && now - flow_link.received_ms < 500U) ||
        (requested_report && now - last_report_request_ms < 1000U)) return;
    requested_report = true;
    last_report_request_ms = now;
    mavlink_msg_command_long_pack_chan(fc_system_id, MAV_COMP_ID_ONBOARD_COMPUTER,
        MAVLINK_COMM_0, &message, fc_system_id, MAV_COMP_ID_AUTOPILOT1,
        MAV_CMD_SET_MESSAGE_INTERVAL, 0, MAVLINK_MSG_ID_OPTICAL_FLOW, 100000,
        0, 0, 0, 0, 0);
    length = mavlink_msg_to_send_buffer(wire, &message);
    HAL_UART_Transmit(&MAV_UART, wire, length, 5);
}
#endif

static void RequestArmStatus(uint32_t now)
{
    static uint32_t last_request_ms;
    if (fc_data.heartbeat_count == 0U ||
        (uint32_t)(now - fc_data.last_heartbeat_ms) >= 3000U ||
        (fc_data.sys_status_count != 0U &&
         (uint32_t)(now - fc_data.last_sys_status_ms) < 3000U) ||
        (uint32_t)(now - last_request_ms) < 5000U) return;
    last_request_ms = now;
    mavlink_message_t message;
    uint8_t packet[MAVLINK_MAX_PACKET_LEN];
    mavlink_msg_command_long_pack_chan(fc_system_id, MAV_COMP_ID_ONBOARD_COMPUTER,
        MAVLINK_COMM_0, &message, fc_system_id, MAV_COMP_ID_AUTOPILOT1,
        MAV_CMD_SET_MESSAGE_INTERVAL, 0, MAVLINK_MSG_ID_SYS_STATUS, 1000000,
        0, 0, 0, 0, 0);
    uint16_t length = mavlink_msg_to_send_buffer(packet, &message);
    HAL_UART_Transmit(&MAV_UART, packet, length, 5);
}

static void PatrolTransmit(uint32_t now)
{
    static uint32_t last_tick, last_stream_request;
    static unsigned stream;
    /* Request the telemetry needed for closed-loop control, not flight actions.
     * Round-robin refresh survives FC restarts and rejected interval requests. */
    if (fc_data.heartbeat_count != 0U && now-fc_data.last_heartbeat_ms < 3000U &&
        now-last_stream_request >= 1000U) {
        static const uint32_t ids[] = {MAVLINK_MSG_ID_LOCAL_POSITION_NED,
            MAVLINK_MSG_ID_ATTITUDE, MAVLINK_MSG_ID_DISTANCE_SENSOR,
            MAVLINK_MSG_ID_EKF_STATUS_REPORT, MAVLINK_MSG_ID_EXTENDED_SYS_STATE};
        mavlink_message_t message;
        uint8_t wire[MAVLINK_MAX_PACKET_LEN];
        unsigned slot=stream++ % 5U;
        mavlink_msg_command_long_pack_chan(fc_system_id, MAV_COMP_ID_ONBOARD_COMPUTER,
            MAVLINK_COMM_0, &message, fc_system_id, MAV_COMP_ID_AUTOPILOT1,
            MAV_CMD_SET_MESSAGE_INTERVAL, 0, (float)ids[slot],
            slot < 3U ? 100000.0f : 500000.0f, 0,0,0,0,0);
        uint16_t size=mavlink_msg_to_send_buffer(wire,&message);
        HAL_UART_Transmit(&MAV_UART,wire,size,5);
        last_stream_request=now;
    }
    if (now-last_tick < 200U) return;
    last_tick=now;
    taskENTER_CRITICAL();
    unsigned request=patrol_request; patrol_request=0;
    taskEXIT_CRITICAL();
    xSemaphoreTake(fc_mutex,portMAX_DELAY);
    PatrolInput input=patrol_input;
    input.armed=fc_data.armed; input.mode=fc_data.flight_mode; input.yaw=fc_data.yaw;
    input.fresh=now-fc_data.last_heartbeat_ms < 2000U &&
        fc_data.sys_status_count != 0U && now-fc_data.last_sys_status_ms < 2500U &&
        patrol_position_ms != 0U && now-patrol_position_ms < 800U &&
        fc_data.attitude_count != 0U && now-fc_data.last_attitude_ms < 800U &&
        patrol_ground_ms != 0U && now-patrol_ground_ms < 1500U;
    input.healthy=input.healthy &&
        (patrol_system_status == MAV_STATE_STANDBY || patrol_system_status == MAV_STATE_ACTIVE);
    input.prearm_ok=fc_data.prearm_supported && fc_data.prearm_enabled && fc_data.prearm_healthy;
    input.nav_ok=patrol_ekf_valid && patrol_ekf_ms != 0U && now-patrol_ekf_ms < 1500U;
    input.tof_ok=patrol_range_valid && patrol_range_ms != 0U && now-patrol_range_ms < 500U &&
        fabsf(fc_data.roll) < 0.5f && fabsf(fc_data.pitch) < 0.5f;
    input.height=patrol_slant_m*cosf(fc_data.roll)*cosf(fc_data.pitch);
    PatrolAction action=Patrol_Update(&patrol,&input,now,request);
    xSemaphoreGive(fc_mutex);
    if (action.type == PATROL_NONE) return;
    mavlink_message_t message;
    uint8_t wire[MAVLINK_MAX_PACKET_LEN];
    if (action.type == PATROL_TARGET) {
        mavlink_msg_set_position_target_local_ned_pack_chan(fc_system_id,
            MAV_COMP_ID_ONBOARD_COMPUTER,MAVLINK_COMM_0,&message,now,fc_system_id,
            MAV_COMP_ID_AUTOPILOT1,MAV_FRAME_LOCAL_NED,0x09f8,
            action.x,action.y,action.z,0,0,0,0,0,0,action.yaw,0);
    } else {
        uint16_t command=action.type == PATROL_SET_MODE ? MAV_CMD_DO_SET_MODE :
            action.type == PATROL_ARM_CMD ? MAV_CMD_COMPONENT_ARM_DISARM : MAV_CMD_NAV_TAKEOFF;
        float p1=action.type == PATROL_SET_MODE ? MAV_MODE_FLAG_CUSTOM_MODE_ENABLED :
            action.type == PATROL_ARM_CMD ? 1.0f : 0.0f;
        mavlink_msg_command_long_pack_chan(fc_system_id,MAV_COMP_ID_ONBOARD_COMPUTER,
            MAVLINK_COMM_0,&message,fc_system_id,MAV_COMP_ID_AUTOPILOT1,command,0,
            p1,action.type == PATROL_SET_MODE ? (float)action.mode : 0.0f,
            0,0,0,0,action.type == PATROL_TAKEOFF_CMD ? action.z : 0.0f);
    }
    uint16_t size=mavlink_msg_to_send_buffer(wire,&message);
    /* No forced arming, parameter writes, origin setting or motor commands. */
    HAL_UART_Transmit(&MAV_UART,wire,size,5);
}

void MAVLink_PatrolRequest(unsigned command)
{
    if (command < 1U || command > 3U) return;
    taskENTER_CRITICAL();
    if (patrol_request == 0U || command != 1U) patrol_request=command;
    taskEXIT_CRITICAL();
}

void MAVLink_PatrolStatus(char *out, uint32_t capacity)
{
    if (!out || !capacity) return;
    if (xSemaphoreTake(fc_mutex,pdMS_TO_TICKS(20)) != pdTRUE) {
        snprintf(out,capacity,"PATROL: telemetry busy\r\n"); return;
    }
    snprintf(out,capacity,"PATROL %s leg=%u: %s\r\n",
        Patrol_StateName(patrol.state),patrol.leg < 4U ? patrol.leg+1U : 4U,patrol.reason);
    xSemaphoreGive(fc_mutex);
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
#if CAMERA_FLOW_MAVLINK
        ForwardCameraFlow((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
#endif
        RequestArmStatus((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        PatrolTransmit((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));

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
