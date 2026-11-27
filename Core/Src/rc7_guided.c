#include "rc7_guided.h"

#include <math.h>

#define GUIDED 4U
#define LAND 9U
#define TAKEOFF_COMMAND 22U
#define HOVER_HEIGHT_M 0.55f
#define LIFTOFF_MIN_RISE_M 0.10f
#define HOVER_BAND_CENTER_M 0.50f
#define HOVER_HEIGHT_TOLERANCE_M 0.10f
#define HOVER_RANGE_CHANGE_M 0.03f
#define MAX_RISE_M 0.85f
#define MAX_HEIGHT_DISAGREEMENT_M 0.40f
#define HOVER_ACQUIRE_TIMEOUT_MS 8000U
#define TAKEOFF_MAX_VERTICAL_SPEED_M_S 0.20f
#define LOW_HOVER_MIN_M 0.30f
#define HEIGHT_ADJUST_STOP_M 0.50f
#define HEIGHT_ADJUST_STEP_M 0.015f
#define HEIGHT_ADJUST_MAX_M 0.20f
#define HEIGHT_ADJUST_STEP_MS 200U
#define HEIGHT_ADJUST_TIMEOUT_MS 8000U
#define FORWARD_DISTANCE_M 1.0f
#define MAX_HORIZONTAL_DISPLACEMENT_M (FORWARD_DISTANCE_M + 0.75f)
#define LEG_TIMEOUT_MS 15000U
#define POSITION_TOLERANCE_M 0.15f
#define POSITION_STEP_M 0.05f

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

static void start_height_adjust(Rc7Guided *test, const PatrolInput *in,
                                Rc7GuidedState resume_state, uint32_t now)
{
    test->state = RC7_HEIGHT_ADJUST;
    test->resume_state = resume_state;
    test->command_x = in->x;
    test->command_y = in->y;
    test->height_adjust_limit_z = in->z - HEIGHT_ADJUST_MAX_M;
    test->target_z = resume_state == RC7_IDLE ? in->z :
        fmaxf(test->height_adjust_limit_z, fminf(test->target_z, in->z));
    test->height_adjust_started_ms = test->height_adjust_ms = now;
    test->settling = false;
}

PatrolAction Rc7Guided_Update(Rc7Guided *test, const PatrolInput *in,
                             uint16_t slant_cm, bool heartbeat_fresh,
                             uint16_t pwm, uint32_t now)
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
        /* Apply the sensor's startup window before projecting range onto vertical. */
        const bool ready = valid && in->on_ground && slant_cm >= 10U &&
            slant_cm <= 35U && fabsf(in->vz) < TAKEOFF_MAX_VERTICAL_SPEED_M_S &&
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
        /* With rangefinder terrain enabled, Guided expects height above the floor. */
        return (PatrolAction){.type = PATROL_TAKEOFF_CMD, .z = HOVER_HEIGHT_M};
    }

    const float initial_height = test->floor_reference - test->origin_z;
    if (test->state != RC7_LANDING &&
        (edge == SWITCH_LOW || !valid || in->height - initial_height > MAX_RISE_M ||
         hypotf(in->x - test->origin_x, in->y - test->origin_y) > MAX_HORIZONTAL_DISPLACEMENT_M ||
         fabsf(in->z - test->origin_z) > 1.10f ||
         fabsf(in->z + in->height - test->floor_reference) > MAX_HEIGHT_DISAGREEMENT_M))
        start_landing(test, now);

    if (test->state == RC7_TAKEOFF &&
        (now - test->started_ms >= 12000U ||
         (in->ack_serial != test->ack_serial && in->ack_command == TAKEOFF_COMMAND &&
          in->ack_result != 0U && in->ack_result != 5U)))
        start_landing(test, now);
    const bool moving = test->state == RC7_FORWARD || test->state == RC7_RETURN;
    const bool adjusting = test->state == RC7_HEIGHT_ADJUST;
    if (adjusting &&
        (now - test->height_adjust_started_ms >= HEIGHT_ADJUST_TIMEOUT_MS || in->on_ground ||
         in->height < LOW_HOVER_MIN_M || in->height >= HOVER_BAND_CENTER_M + HOVER_HEIGHT_TOLERANCE_M))
        start_landing(test, now);
    if ((moving || (adjusting && test->resume_state != RC7_IDLE)) &&
        now - test->started_ms >= LEG_TIMEOUT_MS)
        start_landing(test, now);
    if (moving && (in->on_ground || in->height < LOW_HOVER_MIN_M))
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

    const bool height_in_band = fabsf(in->height - HOVER_BAND_CENTER_M) < HOVER_HEIGHT_TOLERANCE_M;
    const bool low_hover = test->state == RC7_TAKEOFF && !height_in_band &&
        in->height < HOVER_HEIGHT_M && in->height >= LOW_HOVER_MIN_M &&
        in->height - initial_height >= LIFTOFF_MIN_RISE_M &&
        test->stable_switch == SWITCH_MID && test->pending_switch == SWITCH_MID &&
        !test->forward_used;
    const bool stationary = !in->on_ground && (height_in_band || low_hover) &&
        fabsf(in->vz) < 0.15f && hypotf(in->vx, in->vy) < 0.15f;
    if (!stationary) test->settling = false;
    else if (!test->settling || test->settling_low != low_hover ||
             fabsf(in->height - test->settling_height) > HOVER_RANGE_CHANGE_M) {
        test->settling = true;
        test->settling_low = low_hover;
        test->settled_ms = now;
        test->settling_height = in->height;
    }
    const bool settled = test->settling && now - test->settled_ms >= 1000U;
    if (test->state == RC7_TAKEOFF) {
        /* Guided position targets supersede the active takeoff command. */
        if (!settled || test->origin_z - in->z < LIFTOFF_MIN_RISE_M) return none;
        hold_here(test, in);
        test->started_ms = now;
        if (low_hover) {
            start_height_adjust(test, in, RC7_IDLE, now);
        } else {
            test->target_z = in->z + (in->height - HOVER_HEIGHT_M);
        }
    } else if (test->state == RC7_HEIGHT_ADJUST) {
        if (edge == SWITCH_HIGH) {
            test->target_z = in->z;
            hold_here(test, in);
        } else if (in->height >= HEIGHT_ADJUST_STOP_M) {
            /* Drop any accumulated command lead as the measured height approaches target. */
            test->target_z = fmaxf(test->target_z, in->z + (in->height - HOVER_HEIGHT_M));
            if (settled) {
                if (test->resume_state == RC7_IDLE) {
                    hold_here(test, in);
                    test->started_ms = now;
                } else {
                    test->state = test->resume_state;
                    test->settling = false;
                }
            }
        } else if (now - test->height_adjust_ms >= HEIGHT_ADJUST_STEP_MS &&
                   fabsf(in->vz) < 0.15f && hypotf(in->vx, in->vy) < 0.15f &&
                   test->pending_switch == SWITCH_MID) {
            /* NED Z decreases as the target climbs. Keep X/Y fixed in this phase. */
            const float step = fminf(HEIGHT_ADJUST_STEP_M, HOVER_HEIGHT_M - in->height);
            test->target_z = fmaxf(test->height_adjust_limit_z, test->target_z - step);
            test->height_adjust_ms = now;
        }
    } else if (test->state == RC7_FORWARD || test->state == RC7_RETURN) {
        if (edge == SWITCH_HIGH) {
            hold_here(test, in);
        } else if (!height_in_band) {
            if (in->height < HOVER_HEIGHT_M)
                start_height_adjust(test, in, test->state, now);
            else
                hold_here(test, in);
        } else if (settled &&
                   hypotf(in->x - test->target_x, in->y - test->target_y) < POSITION_TOLERANCE_M &&
                   test->command_x == test->target_x && test->command_y == test->target_y) {
            if (test->state == RC7_FORWARD) {
                test->state = RC7_RETURN;
                test->started_ms = now;
                test->settling = false;
                test->target_x = test->origin_x;
                test->target_y = test->origin_y;
            } else {
                test->state = RC7_HOVER;
            }
        }
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
    if (test->state != RC7_HEIGHT_ADJUST &&
        hypotf(test->command_x - in->x, test->command_y - in->y) < POSITION_TOLERANCE_M) {
        if (distance > POSITION_STEP_M) {
            const float fraction = POSITION_STEP_M / distance;
            test->command_x += dx * fraction;
            test->command_y += dy * fraction;
        } else {
            test->command_x = test->target_x;
            test->command_y = test->target_y;
        }
    }
    return (PatrolAction){.type = PATROL_TARGET, .x = test->command_x,
        .y = test->command_y, .z = test->target_z, .yaw = test->yaw};
}
