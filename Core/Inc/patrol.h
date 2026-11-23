#ifndef PATROL_H
#define PATROL_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { PATROL_IDLE, PATROL_MODE, PATROL_ARM, PATROL_TAKEOFF,
    PATROL_LEG, PATROL_TURN, PATROL_HOVER, PATROL_LANDING } PatrolState;
typedef enum { PATROL_NONE, PATROL_SET_MODE, PATROL_ARM_CMD,
    PATROL_TAKEOFF_CMD, PATROL_TARGET } PatrolActionType;
typedef struct {
    bool fresh, healthy, nav_ok, prearm_ok, tof_ok, armed, on_ground;
    uint32_t mode;
    float x, y, z, vx, vy, vz, yaw, height;
    uint32_t ack_serial;
    uint16_t ack_command;
    uint8_t ack_result;
} PatrolInput;
typedef struct {
    PatrolActionType type;
    uint32_t mode;
    float x, y, z, yaw;
} PatrolAction;
typedef struct {
    PatrolState state;
    const char *reason;
    uint32_t entered_ms, last_tick_ms, last_send_ms, settled_ms, healthy_ms;
    uint32_t ack_serial;
    unsigned leg;
    bool sent, settling, health_tracking;
    float origin_x, origin_y, origin_z, initial_yaw, target_x, target_y, target_yaw;
    float command_x, command_y;
} Patrol;

void Patrol_Init(Patrol *p);
/* Commands: 0 tick only, 1 patrol, 2 hold, 3 land. No action on boot. */
PatrolAction Patrol_Update(Patrol *p, const PatrolInput *in, uint32_t now, unsigned command);
const char *Patrol_StateName(PatrolState state);
#endif
