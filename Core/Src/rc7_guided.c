#include "rc7_guided.h"

#include <math.h>

#define GUIDED 4U
#define LAND 9U
#define TAKEOFF_COMMAND 22U
#define TAKEOFF_HEIGHT_M 0.50f
#define FLOOR_HEIGHT_M 0.50f
#define HOVER_HEIGHT_TOLERANCE_M 0.10f
#define HOVER_RANGE_CHANGE_M 0.03f
#define MAX_FLOOR_HEIGHT_M 0.85f
#define HOVER_ACQUIRE_TIMEOUT_MS 8000U
#define TAKEOFF_MAX_VERTICAL_SPEED_M_S 0.20f
#define FORWARD_DISTANCE_M 1.0f
#define MAX_HORIZONTAL_DISPLACEMENT_M (FORWARD_DISTANCE_M + 0.75f)

enum { SWITCH_UNKNOWN, SWITCH_LOW, SWITCH_MID, SWITCH_HIGH };

static uint8_t switch_edge(Rc7Guided *test, uint16_t pwm, uint32_t now)
{
    const uint8_t position = pwm < 1300U ? SWITCH_LOW :
                             pwm > 1700U ? SWITCH_HIGH : SWITCH_MID;
    if (test->pending_switch != position) {
        test->pending_switch = position;
        test->switch_ms = now;
    }
    if (test->stable_switch == position || now - test->switch_ms < 300U)
        return SWITCH_UNKNOWN;
    const bool first = test->stable_switch == SWITCH_UNKNOWN;
    test->stable_switch = position;
    if (position == SWITCH_LOW) test->saw_low = true;
    return first ? SWITCH_UNKNOWN : position;
}

static bool usable(const PatrolInput *in)
{
    return in->fresh && in->healthy && in->nav_ok && in->tof_ok &&
        isfinite(in->x) && isfinite(in->y) && isfinite(in->z) &&
        isfinite(in->vx) && isfinite(in->vy) && isfinite(in->vz) &&
        isfinite(in->height) && isfinite(in->yaw);
}

static void start_landing(Rc7Guided *test, uint32_t now)
{
    test->state = RC7_LANDING;
    test->land_send_ms = now - 1000U;
}

static void hold_here(Rc7Guided *test, const PatrolInput *in)
{
    test->state = RC7_HOVER;
    test->target_x = test->command_x = in->x;
    test->target_y = test->command_y = in->y;
    test->settling = false;
}

PatrolAction Rc7Guided_Update(Rc7Guided *test, const PatrolInput *in,
                             bool heartbeat_fresh, uint16_t pwm, uint32_t now)
{
    const PatrolAction none = {0};
    if (!heartbeat_fresh || pwm < 900U || pwm > 2100U) {
        *test = (Rc7Guided){0};
        return none;
    }

    const uint8_t edge = switch_edge(test, pwm, now);
    if (!in->armed || in->mode != GUIDED) {
        if (test->state != RC7_IDLE || test->stable_switch != SWITCH_LOW)
            test->saw_low = false;
        test->state = RC7_IDLE;
        test->start_qualified = false;
        return none;
    }

    const bool valid = usable(in);
    if (test->state == RC7_IDLE) {
        const bool ready = valid && in->on_ground && in->height >= 0.10f &&
            in->height <= 0.35f && fabsf(in->vz) < TAKEOFF_MAX_VERTICAL_SPEED_M_S &&
            hypotf(in->vx, in->vy) < 0.15f;
        if (ready && !test->start_qualified) test->qualified_ms = now;
        test->start_qualified = ready;
        if (edge != SWITCH_MID || !test->saw_low) return none;
        test->saw_low = false;
        if (!ready || now - test->qualified_ms < 1000U) return none;

        test->state = RC7_TAKEOFF;
        test->started_ms = now;
        test->ack_serial = in->ack_serial;
        test->origin_x = in->x;
        test->origin_y = in->y;
        test->origin_z = in->z;
        test->floor_reference = in->z + in->height;
        test->yaw = in->yaw;
        test->forward_used = false;
        test->settling = false;
        /* Guided interprets NAV_TAKEOFF above terrain or home depending on
         * its rangefinder setup. Start conservatively in either frame. */
        return (PatrolAction){.type = PATROL_TAKEOFF_CMD, .z = TAKEOFF_HEIGHT_M};
    }

    if (test->state != RC7_LANDING &&
        (edge == SWITCH_LOW || !valid || in->height > MAX_FLOOR_HEIGHT_M ||
         hypotf(in->x - test->origin_x, in->y - test->origin_y) > MAX_HORIZONTAL_DISPLACEMENT_M ||
         fabsf(in->z - test->origin_z) > 1.10f ||
         fabsf(in->z + in->height - test->floor_reference) > 0.30f))
        start_landing(test, now);

    if (test->state == RC7_TAKEOFF &&
        (now - test->started_ms >= 12000U ||
         (in->ack_serial != test->ack_serial && in->ack_command == TAKEOFF_COMMAND &&
          in->ack_result != 0U && in->ack_result != 5U)))
        start_landing(test, now);
    if (test->state == RC7_FORWARD && now - test->started_ms >= 15000U)
        start_landing(test, now);
    if (edge == SWITCH_HIGH) test->forward_used = true;
    if (test->state == RC7_HOVER && !test->forward_used &&
        now - test->started_ms >= HOVER_ACQUIRE_TIMEOUT_MS)
        start_landing(test, now);

    if (test->state == RC7_LANDING) {
        if (now - test->land_send_ms < 1000U) return none;
        test->land_send_ms = now;
        return (PatrolAction){.type = PATROL_SET_MODE, .mode = LAND};
    }

    const bool height_in_band = fabsf(in->height - FLOOR_HEIGHT_M) < HOVER_HEIGHT_TOLERANCE_M;
    const bool stationary = !in->on_ground && height_in_band &&
        fabsf(in->vz) < 0.15f && hypotf(in->vx, in->vy) < 0.15f;
    if (!stationary) test->settling = false;
    else if (!test->settling || fabsf(in->height - test->settling_height) > HOVER_RANGE_CHANGE_M) {
        test->settling = true;
        test->settled_ms = now;
        test->settling_height = in->height;
    }
    const bool settled = test->settling && now - test->settled_ms >= 1000U;
    if (test->state == RC7_TAKEOFF) {
        const float initial_height = test->floor_reference - test->origin_z;
        if (in->on_ground || test->origin_z - in->z < 0.10f ||
            in->height - initial_height < 0.10f) return none;
        hold_here(test, in);
        test->started_ms = now;
    } else if (test->state == RC7_FORWARD) {
        if (edge == SWITCH_HIGH || !height_in_band)
            hold_here(test, in);
        else if (settled && hypotf(in->x - test->target_x, in->y - test->target_y) < 0.15f)
            test->state = RC7_HOVER;
    } else {
        if (settled && !test->forward_used &&
            test->stable_switch == SWITCH_MID && test->pending_switch == SWITCH_MID) {
            test->forward_used = true;
            test->state = RC7_FORWARD;
            test->started_ms = now;
            test->yaw = in->yaw;
            test->command_x = in->x;
            test->command_y = in->y;
            test->target_x = in->x + FORWARD_DISTANCE_M * cosf(in->yaw);
            test->target_y = in->y + FORWARD_DISTANCE_M * sinf(in->yaw);
        }
    }

    const float dx = test->target_x - test->command_x;
    const float dy = test->target_y - test->command_y;
    const float distance = hypotf(dx, dy);
    const float fraction = distance > 0.05f ? 0.05f / distance : 1.0f;
    if (hypotf(test->command_x - in->x, test->command_y - in->y) < 0.15f) {
        test->command_x += dx * fraction;
        test->command_y += dy * fraction;
    }
    const float height_correction = fmaxf(-0.10f, fminf(0.10f, in->height - FLOOR_HEIGHT_M));
    return (PatrolAction){.type = PATROL_TARGET, .x = test->command_x,
        .y = test->command_y, .z = in->z + height_correction, .yaw = test->yaw};
}
