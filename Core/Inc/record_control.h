#ifndef RECORD_CONTROL_H
#define RECORD_CONTROL_H
#include <stdbool.h>
#include <stdint.h>

#define RECORD_CAMERA 1U
#define RECORD_THERMAL 2U
#define RECORD_FLOW_LOG 4U
typedef struct {
    uint32_t session, active, pending, ready, done, failed;
    uint32_t start_ms, duration_ms, started;
    uint32_t rx_bytes, rx_errors, rx_overflows, commands, busy_commands;
    uint32_t devices;
    uint32_t automatic, stop_requested;
    uint32_t stop_elapsed_ms;
} RecordReport;
extern volatile RecordReport record_report;
bool RecordControl_Init(void);
/* Read fresh FC arm telemetry; starts synchronized normal/thermal segments. */
void RecordControl_PollArm(void);
/* Freeze a common stop time for both camera files and the video CSV. */
void RecordControl_RequestStop(void);
uint32_t RecordControl_RecordedDuration(void);
bool RecordControl_TakeRequest(uint32_t device);
bool RecordControl_WaitStart(uint32_t device);
bool RecordControl_HasTime(void);
void RecordControl_Done(uint32_t device, bool success);
#endif
