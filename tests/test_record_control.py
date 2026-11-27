"""Run actual UART command parsing and session coordination with a fake clock."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class RecordControl(unittest.TestCase):
    def test_uart_commands_deadline_busy_repeat_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "usart.h").write_text('''
#pragma once
#include <stdint.h>
typedef int HAL_StatusTypeDef;
typedef struct { unsigned RxState; } UART_HandleTypeDef;
extern UART_HandleTypeDef huart7, huart3;
#define HAL_OK 0
#define HAL_UART_STATE_READY 0
#define __DMB() ((void)0)
uint32_t HAL_GetTick(void);
int HAL_UART_Receive_IT(UART_HandleTypeDef *,uint8_t *,unsigned);
''')
            (work / "app_log.h").write_text('''
#include "usart.h"
int AppLog_Write(const void *,uint16_t,uint32_t);
''')
            (work / "cmsis_os2.h").write_text('''
#include <stdint.h>
typedef struct {const char *name;unsigned stack_size,priority;} osThreadAttr_t;
#define osPriorityBelowNormal 16
void osDelay(uint32_t);
void *osThreadNew(void (*)(void *),void *,const osThreadAttr_t *);
''')
            (work / "FreeRTOS.h").write_text('''
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
''')
            (work / "task.h").write_text('')
            (work / "LD6002_UART.h").write_text('')
            source = (ROOT / 'Core/Src/record_control.c').as_posix()
            (work / "exercise.c").write_text('''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#define APP_CAMERA_ENABLED 1
#include "''' + source + '''"
UART_HandleTypeDef huart7, huart3;
volatile ThermalReport thermal_report;
static FC_Telemetry_t fake_fc;
static bool fc_available;
bool MAVLink_GetTelemetry(FC_Telemetry_t *out) {*out=fake_fc; return fc_available;}
static uint32_t now;
static jmp_buf stop;
static int task_steps=-1, partner;
static unsigned patrol_command;
static unsigned uwb_zero_commands;
bool UWB_RangeZeroBearing(void) {++uwb_zero_commands; return true;}
void MAVLink_PatrolRequest(unsigned command) {patrol_command=command;}
void MAVLink_PatrolStatus(char *out,uint32_t capacity) {
    snprintf(out,capacity,"PATROL IDLE\\r\\n");
}
uint32_t HAL_GetTick(void) {return now;}
void osDelay(uint32_t ticks) {
    if(task_steps>=0 && --task_steps==0) longjmp(stop,1);
    now+=ticks;
    if(partner && record_report.ready==RECORD_CAMERA) {
        partner=0;
        assert(RecordControl_WaitStart(RECORD_THERMAL));
    }
}
void *osThreadNew(void (*f)(void *),void *arg,const osThreadAttr_t *attr) {
    (void)f;(void)arg;assert(attr->stack_size>=2048);return (void *)1;
}
int AppLog_Write(const void *p,uint16_t n,uint32_t timeout) {
    (void)p;(void)n;(void)timeout;return 0;
}
int HAL_UART_Receive_IT(UART_HandleTypeDef *u,uint8_t *p,unsigned n) {
    assert(u==&huart7 && p==&received_byte && n==1);return HAL_OK;
}
static void feed(const char *p) {
    while(*p) {received_byte=(uint8_t)*p++;HAL_UART_RxCpltCallback(&huart7);}
}
static void run(int n) {
    task_steps=n;
    if(!setjmp(stop)) CommandTask(NULL);
    task_steps=-1;
}
int main(void) {
    feed("  UwB ZeRo  \\r\\n");run(3);assert(uwb_zero_commands==1);
    feed("uwb zero");run(20);assert(uwb_zero_commands==2);
    assert(RecordControl_Init());
    feed("patrol\\n");run(3);assert(patrol_command==1);
    feed("patrol stop\\n");run(3);assert(patrol_command==2);
    feed("patrol land\\n");run(3);assert(patrol_command==3);
    feed("patrol status\\n");run(3);assert(patrol_command==3);
    feed("patrolxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\n");run(3);
    assert(patrol_command==3); /* Overflow must not launch a patrol. */
    assert(!record_report.active); /* No automatic recording. */
    feed("record\\r\\n");run(3);
    assert(!record_report.active); /* Thermal warmup gates start. */
    thermal_report.ready=1;
    feed("recording\\n");run(3);assert(!record_report.active);
    feed("  ReCoRd  \\r\\n");run(3);
    assert(record_report.active && record_report.session==1);
    assert(RecordControl_TakeRequest(RECORD_CAMERA));
    assert(!RecordControl_TakeRequest(RECORD_CAMERA));
    assert(RecordControl_TakeRequest(RECORD_THERMAL));
    partner=1;assert(RecordControl_WaitStart(RECORD_CAMERA));
    assert(record_report.started && record_report.ready==3);
    uint32_t start=record_report.start_ms;
    now=start+29999;assert(RecordControl_HasTime());
    RecordControl_Done(RECORD_THERMAL,false);
    assert(record_report.failed==RECORD_THERMAL);
    assert(RecordControl_HasTime()); /* Healthy camera keeps the full window. */
    record_report.failed=record_report.done=0;
    now=start+30000;assert(!RecordControl_HasTime());
    feed("record");run(20); /* Idle delimiter, no newline. */
    assert(record_report.busy_commands==1 && record_report.session==1);
    RecordControl_Done(RECORD_CAMERA,true);assert(record_report.active);
    RecordControl_Done(RECORD_THERMAL,true);assert(!record_report.active);
    feed("record");run(20);assert(record_report.session==2);
    assert(record_report.ready==0 && record_report.done==0);
    assert(RecordControl_TakeRequest(RECORD_CAMERA));
    assert(RecordControl_TakeRequest(RECORD_THERMAL));
    RecordControl_Done(RECORD_CAMERA,false);
    assert(!RecordControl_WaitStart(RECORD_THERMAL));
    RecordControl_Done(RECORD_THERMAL,false);assert(!record_report.active);
    feed("recordxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\n");run(3);
    assert(record_report.session==2); /* Truncation cannot create a command. */
    feed("record\\n");run(3);assert(record_report.session==3);
    now=0xfffffff0U;record_report.ready=RECORD_THERMAL;
    assert(RecordControl_WaitStart(RECORD_CAMERA));
    now=record_report.start_ms+29999;assert(RecordControl_HasTime());
    now=record_report.start_ms+30000;assert(!RecordControl_HasTime());
    RecordControl_Done(RECORD_CAMERA,true);
    RecordControl_Done(RECORD_THERMAL,true);
    thermal_report.ready=0;
    feed("record cam");run(20);
    assert(record_report.session==4 && record_report.devices==RECORD_CAMERA);
    assert(!RecordControl_TakeRequest(RECORD_THERMAL));
    assert(RecordControl_TakeRequest(RECORD_CAMERA));
    assert(!RecordControl_WaitStart(RECORD_THERMAL));
    assert(RecordControl_WaitStart(RECORD_CAMERA) && RecordControl_HasTime());
    RecordControl_Done(RECORD_THERMAL,false);
    assert(record_report.active && !record_report.failed);
    RecordControl_Done(RECORD_CAMERA,true);
    assert(!record_report.active && record_report.done==RECORD_CAMERA);
    feed("flow log\\n");run(3);
#if CAMERA_FLOW_MAVLINK
    assert(record_report.session==5 && record_report.devices==RECORD_FLOW_LOG);
    assert(!RecordControl_TakeRequest(RECORD_CAMERA));
    assert(!RecordControl_TakeRequest(RECORD_THERMAL));
    assert(RecordControl_TakeRequest(RECORD_FLOW_LOG));
    assert(RecordControl_WaitStart(RECORD_FLOW_LOG) && RecordControl_HasTime());
    feed("record cam\\n");run(3);
    assert(record_report.session==5 && record_report.busy_commands==2);
    RecordControl_Done(RECORD_FLOW_LOG,true);
    assert(!record_report.active && record_report.done==RECORD_FLOW_LOG);
#else
    assert(record_report.session==4 && !record_report.active);
#endif
    assert(record_report.rx_bytes>0 && record_report.rx_overflows==0);
    uint32_t previous_session=record_report.session;
    now=1000;fc_available=true;
    fake_fc.connected=true;fake_fc.heartbeat_count=1;fake_fc.last_heartbeat_ms=now;
    RecordControl_PollArm();assert(!record_report.active);
    fake_fc.armed=true;RecordControl_PollArm();
    assert(!record_report.active && record_report.session==previous_session); /* Wait for thermal warmup. */
    thermal_report.ready=1;RecordControl_PollArm();
    assert(record_report.automatic && record_report.active && record_report.devices==BOTH);
    assert(record_report.session==previous_session+1 && record_report.duration_ms==30000);
    RecordControl_PollArm();assert(record_report.session==previous_session+1);
    assert(RecordControl_TakeRequest(RECORD_CAMERA));
    assert(RecordControl_TakeRequest(RECORD_THERMAL));
    partner=1;assert(RecordControl_WaitStart(RECORD_CAMERA));
    assert(record_report.ready==BOTH);
    RecordControl_Done(RECORD_CAMERA,true);
    assert(record_report.active);RecordControl_Done(RECORD_THERMAL,true);
    RecordControl_PollArm();assert(record_report.session==previous_session+2);
    RecordControl_Done(RECORD_CAMERA,false);
    RecordControl_Done(RECORD_THERMAL,true);
    RecordControl_PollArm();assert(!record_report.active); /* Failure waits for re-arm. */
    fake_fc.armed=false;RecordControl_PollArm();
    fake_fc.armed=true;fake_fc.last_heartbeat_ms=now;RecordControl_PollArm();
    assert(record_report.active && record_report.session==previous_session+3);
    assert(RecordControl_TakeRequest(RECORD_CAMERA));assert(RecordControl_TakeRequest(RECORD_THERMAL));
    partner=1;assert(RecordControl_WaitStart(RECORD_CAMERA));
    now+=4000;fake_fc.armed=false;RecordControl_PollArm();
    assert(!record_report.stop_requested); /* Stale disarm is not an observed disarm. */
    fake_fc.last_heartbeat_ms=now;RecordControl_PollArm();
    assert(record_report.stop_requested && !RecordControl_HasTime());
    assert(RecordControl_RecordedDuration()==4000);
    now+=2000;RecordControl_RequestStop();assert(RecordControl_RecordedDuration()==4000);
    RecordControl_Done(RECORD_CAMERA,true);RecordControl_Done(RECORD_THERMAL,true);
    RecordControl_PollArm();assert(!record_report.active);
    fake_fc.armed=true;fake_fc.last_heartbeat_ms=now-3000;RecordControl_PollArm();
    assert(!record_report.active); /* Stale arm cannot start a clip. */
    fake_fc.last_heartbeat_ms=now;RecordControl_PollArm();assert(record_report.active);
    fake_fc.armed=false;RecordControl_PollArm();
    assert(!RecordControl_WaitStart(RECORD_CAMERA)); /* Disarmed during preparation. */
    assert(RecordControl_RecordedDuration()==0);
    puts("UART parsing, warmup, synchronization, deadline, busy/repeat, failure and tick wrap passed");
    return 0;
}
''')
            compiler = 'C:/MinGW/bin/gcc.exe' if Path('C:/MinGW/bin/gcc.exe').exists() else shutil.which('gcc')
            executable = work / 'record-control-test.exe'
            for flow_enabled in (0, 1):
                subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                f'-DCAMERA_FLOW_MAVLINK={flow_enabled}',
                                '-I'+str(work), '-I'+str(ROOT / 'Core/Inc'),
                                str(work / 'exercise.c'), '-o', str(executable)], check=True)
                subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
