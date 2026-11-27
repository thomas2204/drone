#include "record_control.h"
#include "thermal_test.h"
#include "app_log.h"
#include "usart.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "mk_app.h"
#include "uwb_range.h"
#include <string.h>
#include <ctype.h>
#include "LD6002_UART.h"


#define RX_SIZE 128U
#define BOTH (RECORD_THERMAL | (APP_CAMERA_ENABLED ? RECORD_CAMERA : 0U))
volatile RecordReport record_report;
/* Single ISR producer, single command-task consumer; debugger visible. */
static volatile uint8_t record_rx[RX_SIZE];
static volatile uint32_t record_rx_head, record_rx_tail;
static uint8_t received_byte;
static bool auto_armed, auto_failed;

static void Log(const char *s) { AppLog_Write(s,(uint16_t)strlen(s),200); }

bool RecordControl_TakeRequest(uint32_t device)
{
    taskENTER_CRITICAL();
    bool pending = (record_report.pending & device) != 0U;
    record_report.pending &= ~device;
    taskEXIT_CRITICAL();
    return pending;
}

bool RecordControl_WaitStart(uint32_t device)
{
    taskENTER_CRITICAL();
    if (!record_report.active || record_report.stop_requested || !(record_report.devices & device)) {
        taskEXIT_CRITICAL();
        return false;
    }
    record_report.ready |= device;
    if (!record_report.started && record_report.ready == record_report.devices && !record_report.failed) {
        record_report.start_ms = HAL_GetTick() + 20U;
        record_report.started = 1;
    }
    taskEXIT_CRITICAL();
    uint32_t waiting = HAL_GetTick();
    while (!record_report.started && !record_report.failed && !record_report.stop_requested) {
        if (HAL_GetTick()-waiting >= 10000U) return false;
        osDelay(1);
    }
    if (record_report.failed || record_report.stop_requested) return false;
    while ((int32_t)(HAL_GetTick()-record_report.start_ms) < 0) osDelay(1);
    return true;
}

bool RecordControl_HasTime(void)
{
    /* Once synchronized, each worker owns its result. A failed peer must
     * not truncate a healthy recording or turn its verification into timeout. */
    return record_report.active && record_report.started &&
        !record_report.stop_requested &&
        HAL_GetTick()-record_report.start_ms < record_report.duration_ms;
}

void RecordControl_Done(uint32_t device, bool success)
{
    taskENTER_CRITICAL();
    if (record_report.active && (record_report.devices & device)) {
        if (!success) record_report.failed |= device;
        record_report.done |= device;
        if (record_report.done == record_report.devices) record_report.active = 0;
    }
    taskEXIT_CRITICAL();
}

static void StartSession(uint32_t devices, bool automatic)
{
    taskENTER_CRITICAL();
    record_report.session++;
    record_report.devices = devices;
    record_report.pending = devices;
    record_report.ready = record_report.done = record_report.failed = 0;
    record_report.start_ms = record_report.started = 0;
    record_report.duration_ms = 30000U;
    record_report.automatic = automatic;
    record_report.stop_requested = 0;
    record_report.active = 1;
    taskEXIT_CRITICAL();
}

void RecordControl_PollArm(void)
{
#if APP_CAMERA_ENABLED
    FC_Telemetry_t fc;
    uint32_t now = HAL_GetTick();
    if (!MAVLink_GetTelemetry(&fc) || !fc.connected || !fc.heartbeat_count ||
        (uint32_t)(now - fc.last_heartbeat_ms) >= 3000U) return;
    if (!fc.armed) {
        auto_armed = auto_failed = false;
        if (record_report.active && record_report.automatic)
            record_report.stop_requested = 1;
        return;
    }
    if (auto_armed && !record_report.active && record_report.automatic && record_report.failed)
        auto_failed = true;
    auto_armed = true;
    if (auto_armed && !auto_failed && !record_report.active) {
        StartSession(RECORD_CAMERA, true);
        Log("REC: armed; preparing camera and synchronized CSV\r\n");
    }
#endif
}

static void Command(char *line)
{
    char *begin=line;
    while (*begin==' ' || *begin=='\t') begin++;
    size_t length=strlen(begin);
    while (length && (begin[length-1]==' ' || begin[length-1]=='\t')) begin[--length]=0;
    if (!length) return;
    for (size_t i=0;i<length;i++) begin[i]=(char)tolower((unsigned char)begin[i]);
    if (!strcmp(begin,"patrol") || !strcmp(begin,"patrol stop") || !strcmp(begin,"patrol land")) {
        MAVLink_PatrolRequest(!strcmp(begin,"patrol") ? 1U :
            !strcmp(begin,"patrol stop") ? 2U : 3U);
        Log("PATROL: request received; checking FC telemetry\r\n"); return;
    }
    if (!strcmp(begin,"patrol status")) {
        char status[160]; MAVLink_PatrolStatus(status,sizeof(status)); Log(status); return;
    }
    if (!strcmp(begin,"uwb zero")) {
        Log(UWB_RangeZeroBearing() ?
            "UWB: forward zero captured; saving to SD\r\n" :
            "UWB: zero failed; wait for fresh filtered phase, then retry\r\n");
        return;
    }
    const bool camera_only = strcmp(begin, "record cam") == 0;
#if CAMERA_FLOW_MAVLINK
    const bool flow_log = strcmp(begin, "flow log") == 0;
#else
    const bool flow_log = false;
#endif
    if (!camera_only && !flow_log && strcmp(begin,"record")) {
#if CAMERA_FLOW_MAVLINK
        Log("Commands: record, record cam, flow log, uwb zero, patrol, patrol stop, patrol land, patrol status\r\n");
#else
        Log("Commands: record, record cam, uwb zero, patrol, patrol stop, patrol land, patrol status\r\n");
#endif
        return;
    }
    if (camera_only && !APP_CAMERA_ENABLED) {
        Log("REC: normal camera unavailable in this build\r\n");
        return;
    }
    const uint32_t devices = flow_log ? RECORD_FLOW_LOG : camera_only ? RECORD_CAMERA : BOTH;
    record_report.commands++;
    if (record_report.active) {
        record_report.busy_commands++;
        Log("REC: busy; wait until recording finishes\r\n"); return;
    }
    if ((devices & RECORD_THERMAL) && !thermal_report.ready) {
        Log("REC: thermal camera not ready; try again shortly\r\n"); return;
    }
    StartSession(devices, false);
    Log(flow_log ? "FLOW LOG: preparing 10-second EKF trace\r\n"
        : camera_only ? "REC: preparing normal camera for 30 seconds\r\n"
        : "REC: preparing both cameras for 30 seconds\r\n");
}

static void CommandTask(void *argument)
{
    (void)argument;
    char line[32];
    unsigned used=0;
    bool overflow=false, announced=false;
    uint32_t last_byte=0, completed_session=0, seen_overflows=0;
    uint32_t last_patrol_status=0;
    char previous_patrol_status[160]={0};
    if (HAL_UART_Receive_IT(&huart7,&received_byte,1)!=HAL_OK) record_report.rx_errors++;
    Log("REC: send record for both cameras or record cam for normal camera only\r\n");
    for (;;) {
        /* Acknowledge the old session before parsing a new command. */
        if (!record_report.active && record_report.session!=completed_session) {
            completed_session=record_report.session; announced=false;
            if (record_report.devices == RECORD_FLOW_LOG)
                Log(record_report.failed ? "FLOW LOG: failed\r\n" : "FLOW LOG: done\r\n");
            else if (record_report.devices == RECORD_CAMERA)
                Log(record_report.failed ? "REC: normal camera recording failed\r\n"
                    : "REC: done; normal camera file saved\r\n");
            else if (record_report.devices == RECORD_THERMAL)
                Log(record_report.failed ? "REC: thermal recording failed\r\n"
                    : "REC: done; thermal file saved\r\n");
            else Log(record_report.failed==RECORD_THERMAL ? "REC: normal camera saved; thermal recording failed\r\n"
                : record_report.failed==RECORD_CAMERA ? "REC: thermal saved; normal camera recording failed\r\n"
                : record_report.failed ? "REC: both recordings failed; check camera messages\r\n"
                : "REC: done; both recordings saved\r\n");
        }
        RecordControl_PollArm();
        if (record_report.rx_overflows!=seen_overflows) {
            seen_overflows=record_report.rx_overflows; overflow=true;
        }
        while (record_rx_tail!=record_rx_head) {
            uint8_t c=record_rx[record_rx_tail];
            record_rx_tail=(record_rx_tail+1U)%RX_SIZE;
            last_byte=HAL_GetTick();
            if (c=='\r' || c=='\n') {
                if (!overflow) { line[used]=0; Command(line); }
                used=0; overflow=false;
            } else if (c==8 || c==127) { if (used) used--; }
            else if (c>=32 && c<127) {
                if (used<sizeof(line)-1U) line[used++]=(char)c;
                else overflow=true;
            } else if (c!='\t') overflow=true;
        }
        /* Bluetooth terminal apps can send a command without CR/LF. */
        if ((used || overflow) && HAL_GetTick()-last_byte>=150U) {
            if (!overflow) { line[used]=0; Command(line); }
            used=0; overflow=false;
        }
        if (record_report.started && record_report.active && !announced) {
            Log(record_report.devices == RECORD_FLOW_LOG ? "FLOW LOG: collecting for 10 seconds\r\n"
                : record_report.devices == RECORD_CAMERA ? "REC: recording normal camera\r\n"
                : record_report.devices == RECORD_THERMAL ? "REC: recording thermal camera\r\n"
                : "REC: recording both cameras\r\n");
            announced=true;
        }
        if (HAL_GetTick()-last_patrol_status >= 1000U) {
            char status[160]; MAVLink_PatrolStatus(status,sizeof(status));
            if (strcmp(status,previous_patrol_status)) {
                Log(status); memcpy(previous_patrol_status,status,strlen(status)+1U);
            }
            last_patrol_status=HAL_GetTick();
        }
        osDelay(10);
    }
}

bool RecordControl_Init(void)
{
    const osThreadAttr_t attr={.name="record_command",.stack_size=2048,.priority=osPriorityBelowNormal};
    return osThreadNew(CommandTask,NULL,&attr)!=NULL;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    if (uart!=&huart7) return;
    uint32_t next=(record_rx_head+1U)%RX_SIZE;
    record_report.rx_bytes++;
    if (next==record_rx_tail) record_report.rx_overflows++;
    else { record_rx[record_rx_head]=received_byte; __DMB(); record_rx_head=next; }
    if (HAL_UART_Receive_IT(&huart7,&received_byte,1)!=HAL_OK) record_report.rx_errors++;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart == &huart3)
        return;                 /* LD6002_Poll() restarts reception in task context */

    if (uart != &huart7) return;
    record_report.rx_errors++;
    if (uart->RxState == HAL_UART_STATE_READY)
        (void)HAL_UART_Receive_IT(uart, &received_byte, 1);
}
