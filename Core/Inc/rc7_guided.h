#ifndef RC7_GUIDED_H
#define RC7_GUIDED_H

#include "patrol.h"

typedef enum {
    RC7_IDLE,
    RC7_TAKEOFF,
    RC7_HOVER,
    RC7_FORWARD,
    RC7_LANDING
} Rc7GuidedState;

typedef struct {
    Rc7GuidedState state;
    uint8_t pending_switch, stable_switch;
    bool saw_low, forward_used, settling, start_qualified;
    uint32_t switch_ms, started_ms, settled_ms, qualified_ms, land_send_ms, ack_serial;
    float origin_x, origin_y, origin_z, floor_reference;
    float settling_height;
    float target_x, target_y, yaw, command_x, command_y;
} Rc7Guided;

/* Zero-initialize at boot. A zero PWM means RC telemetry is unavailable. */
PatrolAction Rc7Guided_Update(Rc7Guided *test, const PatrolInput *input,
                             bool heartbeat_fresh, uint16_t pwm, uint32_t now);

#endif
