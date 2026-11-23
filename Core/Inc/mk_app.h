
#ifndef MK_APP_H
#define MK_APP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    bool connected;
    bool armed;
    bool prearm_supported;
    bool prearm_enabled;
    bool prearm_healthy;
    uint32_t sys_status_count;
    uint32_t last_sys_status_ms;

    uint32_t flight_mode;
    uint32_t heartbeat_count;
    uint32_t attitude_count;
    uint32_t gps_tx_count;       // Successful local UART transmissions, not FC acknowledgments.
    uint32_t gps_tx_errors;
    uint8_t gps_tx_fix_type;

    float roll;       // radians
    float pitch;      // radians
    float yaw;        // radians

    float roll_rate;  // rad/s
    float pitch_rate;
    float yaw_rate;

    uint32_t last_heartbeat_ms;
    uint32_t last_attitude_ms;

} FC_Telemetry_t;

// Initialize before creating/starting tasks
void MAVLink_AppInit(void);

// Main FreeRTOS worker, runs indefinitely
void MAVLink_Task(void);

// Safe to call from other FreeRTOS tasks
bool MAVLink_GetTelemetry(FC_Telemetry_t *out);

// Raw received bytes, for debugging
uint32_t MAVLink_GetRxByteCount(void);

/* Queued only; all MAVLink flight transmissions remain owned by MAVLink_Task.
 * Commands: 1 patrol, 2 stop/hover, 3 land. Status text is a constant string. */
void MAVLink_PatrolRequest(unsigned command);
void MAVLink_PatrolStatus(char *out, uint32_t capacity);

#endif
