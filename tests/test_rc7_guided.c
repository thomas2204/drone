#include "rc7_guided.h"

#include <assert.h>
#include <math.h>

static PatrolAction tick(Rc7Guided *test, PatrolInput *in, uint16_t pwm,
                         uint32_t *now)
{
    *now += 200U;
    return Rc7Guided_Update(test, in, true, pwm, *now);
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

    in.on_ground = false;
    in.vz = 0.0f;
    in.height = 0.43f;
    in.z = -0.28f;
    action = tick(&test, &in, 1500U, &now);
    assert(action.type == PATROL_TARGET && action.z < in.z);
    for (unsigned i = 0; i < 8U; ++i) {
        in.height = i % 2U == 0U ? 0.55f : 0.45f;
        in.z = 0.15f - in.height;
        action = tick(&test, &in, 1500U, &now);
        assert(test.state == RC7_HOVER && action.y == 0.0f);
    }
    in.height = 0.50f;
    in.z = -0.35f;
    in.vz = 0.18f;
    for (unsigned i = 0; i < 7U; ++i) {
        action = tick(&test, &in, 1500U, &now);
        assert(test.state == RC7_HOVER && action.y == 0.0f);
    }
    in.vz = 0.0f;
    Rc7Guided cancelled = test;
    uint32_t cancel_now = now;
    for (unsigned i = 0; i < 4U; ++i) tick(&cancelled, &in, 2000U, &cancel_now);
    for (unsigned i = 0; i < 10U; ++i)
        assert(tick(&cancelled, &in, 1500U, &cancel_now).y == 0.0f);
    assert(cancelled.state == RC7_HOVER && cancelled.forward_used);

    Rc7Guided timeout = test;
    in.height = 0.70f;
    in.z = -0.55f;
    action = Rc7Guided_Update(&timeout, &in, true, 1500U, timeout.started_ms + 8000U);
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
    in.height = 0.86f;
    in.z = 0.15f - in.height;
    action = tick(&too_high, &in, 1500U, &interrupt_now);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    in.height = 0.50f;
    in.z = -0.35f;
    for (unsigned i = 0; i < 55U; ++i) {
        action = tick(&test, &in, 1500U, &now);
        assert(action.type == PATROL_TARGET && fabsf(action.x) < 0.001f);
        assert(action.y - in.y <= 0.051f);
        in.x = action.x;
        in.y = action.y;
    }
    assert(test.state == RC7_HOVER && test.forward_used && fabsf(in.y - 1.0f) < 0.01f);
    for (unsigned i = 0; i < 4U; ++i) tick(&test, &in, 1500U, &now);
    for (unsigned i = 0; i < 8U; ++i)
        assert(fabsf(tick(&test, &in, 2000U, &now).y - 1.0f) < 0.01f);

    Rc7Guided overshoot = test;
    in.y = 1.80f;
    action = tick(&overshoot, &in, 2000U, &now);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    in.y = 1.0f;

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
    assert(Rc7Guided_Update(&lost, &in, false, 0U, now).type == PATROL_NONE);
    in.armed = false;
    hold_switch(&lost, &in, 1500U, &now, 6U);
    in.armed = true;

    for (unsigned i = 0; i < 2U; ++i) tick(&test, &in, 1000U, &now);
    action = tick(&test, &in, 1000U, &now);
    assert(action.type == PATROL_SET_MODE && action.mode == 9U);
    return 0;
}
