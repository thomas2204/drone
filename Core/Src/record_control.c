#include "record_control.h"
#include "thermal_test.h"
#include "app_log.h"
#include "usart.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <ctype.h>

#define RX_SIZE 128U
#define BOTH (RECORD_THERMAL | (APP_CAMERA_ENABLED ? RECORD_CAMERA : 0U))
volatile RecordReport record_report;
/* Single ISR producer, single command-task consumer; debugger visible. */
static volatile uint8_t record_rx[RX_SIZE];
static volatile uint32_t record_rx_head, record_rx_tail;
static uint8_t received_byte;

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
    record_report.ready |= device;
    if (record_report.ready == BOTH && !record_report.failed) {
        record_report.start_ms = HAL_GetTick() + 20U;
        record_report.started = 1;
    }
    taskEXIT_CRITICAL();
    uint32_t waiting = HAL_GetTick();
    while (!record_report.started && !record_report.failed) {
        if (HAL_GetTick()-waiting >= 10000U) return false;
        osDelay(1);
    }
    if (record_report.failed) return false;
    while ((int32_t)(HAL_GetTick()-record_report.start_ms) < 0) osDelay(1);
    return true;
}

bool RecordControl_HasTime(void)
{
    return record_report.active && record_report.started && !record_report.failed &&
        HAL_GetTick()-record_report.start_ms < record_report.duration_ms;
}

void RecordControl_Done(uint32_t device, bool success)
{
    taskENTER_CRITICAL();
    if (!success) record_report.failed |= device;
    record_report.done |= device;
    if (record_report.done == BOTH) record_report.active = 0;
    taskEXIT_CRITICAL();
}

static void Command(char *line)
{
    char *begin=line;
    while (*begin==' ' || *begin=='\t') begin++;
    size_t length=strlen(begin);
    while (length && (begin[length-1]==' ' || begin[length-1]=='\t')) begin[--length]=0;
    if (!length) return;
    for (size_t i=0;i<length;i++) begin[i]=(char)tolower((unsigned char)begin[i]);
    if (strcmp(begin,"record")) { Log("REC: unknown command; send record\r\n"); return; }
    record_report.commands++;
    if (record_report.active) {
        record_report.busy_commands++;
        Log("REC: busy; wait until both files are saved\r\n"); return;
    }
    if (!thermal_report.ready) { Log("REC: thermal camera not ready; try again shortly\r\n"); return; }
    taskENTER_CRITICAL();
    record_report.session++;
    record_report.pending=BOTH;
    record_report.ready=record_report.done=record_report.failed=0;
    record_report.start_ms=record_report.started=0;
    record_report.duration_ms=10000U;
    record_report.active=1;
    taskEXIT_CRITICAL();
    Log("REC: preparing both cameras for 10 seconds\r\n");
}

static void CommandTask(void *argument)
{
    (void)argument;
    char line[32];
    unsigned used=0;
    bool overflow=false, announced=false;
    uint32_t last_byte=0, completed_session=0, seen_overflows=0;
    if (HAL_UART_Receive_IT(&huart7,&received_byte,1)!=HAL_OK) record_report.rx_errors++;
    Log("REC: send record to capture both cameras for 10 seconds\r\n");
    for (;;) {
        /* Acknowledge the old session before parsing a new command. */
        if (!record_report.active && record_report.session!=completed_session) {
            completed_session=record_report.session; announced=false;
            Log(record_report.failed ? "REC: finished with errors; check camera messages\r\n"
                : "REC: done; both 10-second files saved\r\n");
        }
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
            Log("REC: recording both cameras for 10 seconds\r\n"); announced=true;
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
    if (uart!=&huart7) return;
    record_report.rx_errors++;
    if (uart->RxState==HAL_UART_STATE_READY)
        (void)HAL_UART_Receive_IT(uart,&received_byte,1);
}
