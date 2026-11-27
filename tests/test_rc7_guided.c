#include "rc7_guided.h"

#include <assert.h>
#include <math.h>

static PatrolAction tick(Rc7Guided *test, PatrolInput *in, uint16_t pwm,
                         uint32_t *now)
{
    *now += 200U;
    return Rc7Guided_Update(test, in, (uint16_t)lroundf(in->height * 100.0f),
                            true, pwm, *now);
}

static void hold_switch(Rc7Guided *test, PatrolInput *in, uint16_t pwm,
                         uint32_t *now, unsigned ticks)
{
    for (unsigned i = 0; i < ticks; ++i)
        assert(tick(test, in, pwm, now).type == PATROL_NONE);
}

int main(void)
{
    Rc7Guided test = {0};
    PatrolInput in = {.fresh = true, .healthy = true, .nav_ok = true,
        .tof_ok = true, .on_ground = true, .mode = 4U, .height = 0.15f,
        .yaw = 1.5707963f};
    uint32_t now = UINT32_MAX - 1000U;

    /* No action on boot at High or when arming with the switch at Mid. */
    hold_switch(&test, &in, 2000U, &now, 6U);
    hold_switch(&test, &in, 1500U, &now, 4U);
    in.armed = true;
    hold_switch(&test, &in, 1500U, &now, 8U);
    assert(test.state == RC7_IDLE);

    /* The 10 cm sensor reading projects slightly below 10 cm after tilt correction. */
    Rc7Guided minimum_range = test;
    Rc7Guided below_minimum = test;
    uint32_t boundary_now = now;
    in.height = 0.10f * cosf(0.31f * 0.0174532925f);
    for (unsigned i = 0; i < 11U; ++i) {
        boundary_now += 200U;
        uint16_t pwm = i < 8U ? 1000U : 1500U;
        PatrolAction at_minimum = Rc7Guided_Update(&minimum_range, &in, 10U, true, pwm, boundary_now);
        PatrolAction below = Rc7Guided_Update(&below_minimum, &in, 9U, true, pwm, boundary_now);
        assert(at_minimum.type == (i == 10U ? PATROL_TAKEOFF_CMD : PATROL_NONE));
        if (i == 10U) assert(fabsf(at_minimum.z - 0.50f) < 0.001f);
        assert(below.type == PATROL_NONE);
    }
    in.height = 0.15f;

    Rc7Guided too_fast = test;
    in.vz = -0.20f;
    hold_switch(&too_fast, &in, 1000U, &now, 8U);
    hold_switch(&too_fast, &in, 1500U, &now, 4U);
    assert(too_fast.state == RC7_IDLE);

    in.vz = 0.18f;
    hold_switch(&test, &in, 1500U, &now, 6U);
    hold_switch(&test, &in, 1000U, &now, 4U);
    hold_switch(&test, &in, 1500U, &now, 2U);
    PatrolAction action = tick(&test, &in, 1500U, &now);
    assert(action.type == PATROL_TAKEOFF_CMD && fabsf(action.z - 0.50f) < 0.001f);
    hold_switch(&test, &in, 1500U, &now, 4U);
    assert(test.state == RC7_TAKEOFF);

    Rc7Guided false_climb = test;
    PatrolInput false_height = in;
    false_height.z = -0.39f;
    action = Rc7Guided_Update(&false_climb, &false_height, 15U, true, 1500U, now + 200U);
    assert(action.type == PATROL_NONE && false_climb.state == RC7_TAKEOFF);
    false_height.z = -0.41f;
    action = Rc7Guided_Update(&false_climb, &false_height, 15U, true, 1500U, now + 400U);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);

    in.on_ground = false;
    in.vz = 0.0f;
    in.height = 0.19f;
    in.z = -0.04f;
    assert(tick(&test, &in, 1500U, &now).type == PATROL_NONE);
    assert(test.state == RC7_TAKEOFF);
    in.height = 0.30f;
    in.z = -0.15f;
    action = tick(&test, &in, 1500U, &now);
    assert(action.type == PATROL_NONE && test.state == RC7_TAKEOFF);
    in.height = 0.39f;
    in.z = -0.24f;
    action = tick(&test, &in, 1500U, &now);
    assert(action.type == PATROL_NONE && test.state == RC7_TAKEOFF);
    Rc7Guided incomplete_takeoff = test;
    action = Rc7Guided_Update(&incomplete_takeoff, &in, 39U, true, 1500U,
                              test.started_ms + 12000U);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);

    Rc7Guided low_hover = test;
    low_hover.settling = false;
    PatrolInput low = in;
    uint32_t low_now = now;
    low.height = 0.36f;
    low.z = -0.21f;
    low.vz = -0.12f; /* Log 2 reported a climb during the low hover. */
    hold_switch(&low_hover, &low, 1500U, &low_now, 5U);
    action = tick(&low_hover, &low, 1500U, &low_now);
    assert(low_hover.state == RC7_HEIGHT_ADJUST && action.type == PATROL_TARGET);
    assert(action.x == low.x && action.y == low.y && action.z == low.z);
    float previous_z = action.z;
    for (unsigned i = 0; i < 20U; ++i) {
        action = tick(&low_hover, &low, 1500U, &low_now);
        assert(action.type == PATROL_TARGET && low_hover.state == RC7_HEIGHT_ADJUST);
        assert(action.x == low.x && action.y == low.y);
        assert(action.z <= previous_z && previous_z - action.z <= 0.0151f);
        assert(low.z - action.z <= 0.2001f);
        previous_z = action.z;
    }
    assert(fabsf(low.z - action.z - 0.20f) < 0.001f);
    Rc7Guided stuck_low = low_hover;
    action = Rc7Guided_Update(&stuck_low, &low, 36U, true, 1500U,
                              low_hover.started_ms + 8000U);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);

    Rc7Guided overshot_height = low_hover;
    PatrolInput high = low;
    high.height = 0.61f;
    high.z = -0.46f;
    action = Rc7Guided_Update(&overshot_height, &high, 61U, true, 1500U, low_now + 200U);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);

    Rc7Guided cancel_adjust = low_hover;
    uint32_t adjust_cancel_now = low_now;
    for (unsigned i = 0; i < 3U; ++i)
        action = tick(&cancel_adjust, &low, 2000U, &adjust_cancel_now);
    assert(cancel_adjust.state == RC7_HOVER && cancel_adjust.forward_used && action.z == low.z);

    low.height = 0.49f;
    low.z = -0.34f;
    low.vz = 0.0f;
    for (unsigned i = 0; i < 5U; ++i) {
        action = tick(&low_hover, &low, 1500U, &low_now);
        assert(low_hover.state == RC7_HEIGHT_ADJUST && fabsf(action.z + 0.35f) < 0.001f);
    }
    action = tick(&low_hover, &low, 1500U, &low_now);
    assert(low_hover.state == RC7_HOVER && action.x == low.x && action.y == low.y);
    for (unsigned i = 0; i < 5U; ++i) {
        action = tick(&low_hover, &low, 1500U, &low_now);
        assert(low_hover.state == RC7_HOVER && action.x == low.x && action.y == low.y);
    }
    action = tick(&low_hover, &low, 1500U, &low_now);
    assert(low_hover.state == RC7_FORWARD && action.y > low.y);

    for (unsigned i = 0; i < 8U; ++i) {
        in.height = i % 2U == 0U ? 0.55f : 0.45f;
        in.z = 0.15f - in.height;
        action = tick(&test, &in, 1500U, &now);
        assert(test.state == RC7_TAKEOFF && action.type == PATROL_NONE);
    }
    in.height = 0.50f;
    in.z = -0.35f;
    in.vz = 0.18f;
    for (unsigned i = 0; i < 7U; ++i) {
        action = tick(&test, &in, 1500U, &now);
        assert(test.state == RC7_TAKEOFF && action.type == PATROL_NONE);
    }
    in.vz = 0.0f;
    hold_switch(&test, &in, 1500U, &now, 5U);
    action = tick(&test, &in, 1500U, &now);
    assert(test.state == RC7_HOVER && action.type == PATROL_TARGET);
    const float hover_z = action.z;
    Rc7Guided drifting = test;
    in.height = 0.53f;
    in.z = -0.45f;
    action = Rc7Guided_Update(&drifting, &in, 53U, true, 1500U, now + 200U);
    assert(action.type == PATROL_TARGET && action.z == hover_z);
    in.height = 0.50f;
    in.z = -0.35f;

    Rc7Guided cancelled = test;
    uint32_t cancel_now = now;
    for (unsigned i = 0; i < 4U; ++i) tick(&cancelled, &in, 2000U, &cancel_now);
    for (unsigned i = 0; i < 10U; ++i)
        assert(tick(&cancelled, &in, 1500U, &cancel_now).y == 0.0f);
    assert(cancelled.state == RC7_HOVER && cancelled.forward_used);

    Rc7Guided timeout = test;
    in.height = 0.35f;
    in.z = -0.20f;
    action = Rc7Guided_Update(&timeout, &in, 35U, true, 1500U, timeout.started_ms + 8000U);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    in.height = 0.50f;
    in.z = -0.35f;
    for (unsigned i = 0; i < 5U; ++i) {
        action = tick(&test, &in, 1500U, &now);
        assert(test.state == RC7_HOVER && action.y == 0.0f &&
               fabsf(action.z - in.z) < 0.001f);
    }
    action = tick(&test, &in, 1500U, &now);
    assert(test.state == RC7_FORWARD && action.y > 0.0f);
    in.y = action.y;
    Rc7Guided interrupted = test;
    uint32_t interrupt_now = now;
    for (unsigned i = 0; i < 3U; ++i)
        action = tick(&interrupted, &in, 2000U, &interrupt_now);
    assert(interrupted.state == RC7_HOVER && action.y == in.y);

    Rc7Guided too_high = test;
    in.height = 1.01f;
    in.z = 0.15f - in.height;
    action = tick(&too_high, &in, 1500U, &interrupt_now);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    in.height = 0.50f;
    in.z = -0.35f;
    for (unsigned i = 0; i < 30U && test.state == RC7_FORWARD; ++i) {
        action = tick(&test, &in, 1500U, &now);
        assert(action.type == PATROL_TARGET && fabsf(action.x) < 0.001f);
        assert(fabsf(action.y - in.y) <= 0.051f);
        in.x = action.x;
        in.y = action.y;
    }
    assert(test.state == RC7_RETURN && test.forward_used && in.y >= 0.94f);
    assert(test.target_x == test.origin_x && test.target_y == test.origin_y);

    Rc7Guided return_timeout = test;
    action = Rc7Guided_Update(&return_timeout, &in, 50U, true, 1500U,
                              test.started_ms + 15000U);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);

    Rc7Guided return_cancelled = test;
    cancel_now = now;
    for (unsigned i = 0; i < 3U; ++i)
        action = tick(&return_cancelled, &in, 2000U, &cancel_now);
    assert(return_cancelled.state == RC7_HOVER && action.y == in.y);

    for (unsigned i = 0; i < 30U && test.state == RC7_RETURN; ++i) {
        action = tick(&test, &in, 1500U, &now);
        assert(action.type == PATROL_TARGET && fabsf(action.x) < 0.001f);
        assert(action.y <= in.y && in.y - action.y <= 0.051f);
        assert(action.z == hover_z && action.yaw == in.yaw);
        in.x = action.x;
        in.y = action.y;
    }
    assert(test.state == RC7_HOVER && test.forward_used && fabsf(in.y) < 0.01f);
    for (unsigned i = 0; i < 4U; ++i) tick(&test, &in, 1500U, &now);
    for (unsigned i = 0; i < 8U; ++i)
        assert(fabsf(tick(&test, &in, 2000U, &now).y) < 0.01f);

    Rc7Guided overshoot = test;
    in.y = 1.80f;
    action = tick(&overshoot, &in, 2000U, &now);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    in.y = 0.0f;

    Rc7Guided takeover = test;
    in.mode = 5U;
    assert(tick(&takeover, &in, 2000U, &now).type == PATROL_NONE);
    assert(takeover.state == RC7_IDLE);
    in.mode = 4U;
    hold_switch(&takeover, &in, 2000U, &now, 6U);

    Rc7Guided fault = test;
    in.tof_ok = false;
    action = tick(&fault, &in, 2000U, &now);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    in.tof_ok = true;

    Rc7Guided lost = test;
    assert(Rc7Guided_Update(&lost, &in, 50U, false, 0U, now).type == PATROL_NONE);
    in.armed = false;
    hold_switch(&lost, &in, 1500U, &now, 6U);
    in.armed = true;

    for (unsigned i = 0; i < 2U; ++i) tick(&test, &in, 1000U, &now);
    action = tick(&test, &in, 1000U, &now);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    return 0;
}
