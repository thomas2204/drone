#ifndef RECORD_CONTROL_H
#define RECORD_CONTROL_H
#include <stdbool.h>
#include <stdint.h>

#define RECORD_CAMERA 1U
#define RECORD_THERMAL 2U
typedef struct {
    uint32_t session, active, pending, ready, done, failed;
    uint32_t start_ms, duration_ms, started;
    uint32_t rx_bytes, rx_errors, rx_overflows, commands, busy_commands;
} RecordReport;
extern volatile RecordReport record_report;
bool RecordControl_Init(void);
bool RecordControl_TakeRequest(uint32_t device);
bool RecordControl_WaitStart(uint32_t device);
bool RecordControl_HasTime(void);
void RecordControl_Done(uint32_t device, bool success);
#endif
