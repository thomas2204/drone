
#ifndef MAVLINK_APP_H
#define MAVLINK_APP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    bool connected;
    bool armed;

    uint32_t flight_mode;
    uint32_t heartbeat_count;
    uint32_t attitude_count;

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

#endif
