#include "patrol.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846f
#define GUIDED 4U
#define LAND 9U

static float wrap(float a) { return atan2f(sinf(a), cosf(a)); }
static float clamp(float a, float limit) { return fmaxf(-limit, fminf(a, limit)); }
static bool finite_input(const PatrolInput *i)
{
    return isfinite(i->x) && isfinite(i->y) && isfinite(i->z) &&
        isfinite(i->vx) && isfinite(i->vy) && isfinite(i->vz) &&
        isfinite(i->yaw) && isfinite(i->height);
}
static void enter(Patrol *p, PatrolState s, uint32_t now, const char *reason)
{
    p->state = s; p->entered_ms = now; p->sent = p->settling = false;
    p->reason = reason;
}
void Patrol_Init(Patrol *p) { memset(p, 0, sizeof(*p)); p->reason = "idle"; }
const char *Patrol_StateName(PatrolState s)
{
    static const char *names[] = {"IDLE", "GUIDED", "ARMING", "TAKEOFF",
        "LEG", "TURN", "HOVER", "LANDING"};
    return (unsigned)s < sizeof(names)/sizeof(names[0]) ? names[s] : "INVALID";
}
static const char *blocked(const PatrolInput *i)
{
    if (!i->fresh || !finite_input(i)) return "FC telemetry missing/stale";
    if (!i->healthy) return "FC reports unhealthy sensors/failsafe";
    if (!i->nav_ok) return "EKF position/velocity not ready";
    if (!i->tof_ok) return "downward TFmini missing/invalid/stale";
    return NULL;
}
static bool settled(Patrol *p, bool condition, uint32_t now)
{
    if (!condition) { p->settling = false; return false; }
    if (!p->settling) { p->settling = true; p->settled_ms = now; }
    return (uint32_t)(now - p->settled_ms) >= 2000U;
}
static void corner(Patrol *p)
{
    /* Fixed corners, rotated by heading at launch: 10 forward, 5 right,
     * 10 back, 5 left. Never re-anchor corners to noisy current GPS. */
    static const float f[] = {10,10,0,0}, r[] = {0,5,5,0};
    float c = cosf(p->initial_yaw), s = sinf(p->initial_yaw);
    p->target_x = p->origin_x + c*f[p->leg] - s*r[p->leg];
    p->target_y = p->origin_y + s*f[p->leg] + c*r[p->leg];
}

PatrolAction Patrol_Update(Patrol *p, const PatrolInput *i, uint32_t now, unsigned command)
{
    PatrolAction a = {0};
    uint32_t elapsed = now - p->last_tick_ms;
    p->last_tick_ms = now;
    const char *why = blocked(i);
    bool good = !why && i->prearm_ok && !i->armed && i->on_ground;
    if (good && !p->health_tracking) { p->healthy_ms = now; p->health_tracking = true; }
    if (!good) p->health_tracking = false;

    if (command == 1U) {
        if (p->state != PATROL_IDLE) p->reason = "busy; patrol not restarted";
        else if (why) p->reason = why;
        else if (i->armed || !i->on_ground) p->reason = "start requires DISARMED on ground";
        else if (i->height > 1.0f) p->reason = "start requires TFmini ground clearance below 1m";
        else if (!i->prearm_ok) p->reason = "FC pre-arm checks failing/disabled/unknown";
        else if (!p->health_tracking || (uint32_t)(now-p->healthy_ms) < 10000U)
            p->reason = "need 10 seconds of continuously healthy telemetry";
        else {
            p->origin_x = i->x; p->origin_y = i->y; p->origin_z = i->z;
            p->initial_yaw = p->target_yaw = i->yaw;
            p->target_x = i->x; p->target_y = i->y; p->leg = 0;
            p->command_x = i->x; p->command_y = i->y;
            enter(p, PATROL_MODE, now, "requesting Guided");
        }
    }
    if (p->state == PATROL_IDLE) return a;

    /* A pilot/FC mode change ends ownership. Never re-enter Guided to fight it. */
    if (p->state != PATROL_MODE && i->fresh && i->mode != GUIDED) {
        enter(p, PATROL_IDLE, now, "FC/pilot changed mode; patrol cancelled"); return a;
    }
    if (p->state >= PATROL_TAKEOFF && i->fresh && !i->armed) {
        enter(p, PATROL_IDLE, now, "FC disarmed; patrol cancelled"); return a;
    }
    if (command == 3U || why) {
        if (!i->armed) { enter(p, PATROL_IDLE, now, why ? why : "cancelled before takeoff"); return a; }
        enter(p, PATROL_LANDING, now, why ? why : "land requested");
    }
    if (command == 2U && !why && i->armed && p->state >= PATROL_TAKEOFF && p->state != PATROL_LANDING) {
        p->target_x = i->x; p->target_y = i->y; p->target_yaw = i->yaw;
        p->command_x = i->x; p->command_y = i->y;
        enter(p, PATROL_HOVER, now, "hold requested");
    } else if (command == 2U && p->state < PATROL_TAKEOFF) {
        if (i->armed) enter(p, PATROL_LANDING, now, "cancelled during arming");
        else { enter(p, PATROL_IDLE, now, "cancelled before arming"); return a; }
    }
    if (!i->fresh) return a; /* FC link-loss failsafes retain authority. */
    if (p->state == PATROL_LANDING) {
        a.type = PATROL_SET_MODE; a.mode = LAND; return a;
    }
    if (p->state >= PATROL_TAKEOFF &&
        (hypotf(i->x-p->origin_x, i->y-p->origin_y) > 20.0f ||
         i->height > 4.5f || fabsf(i->z-p->origin_z) > 6.0f)) {
        enter(p, PATROL_LANDING, now, "patrol distance/height bound exceeded");
        a.type = PATROL_SET_MODE; a.mode = LAND; return a;
    }
    if (p->state == PATROL_MODE) {
        if (!i->prearm_ok) { enter(p, PATROL_IDLE, now, "pre-arm readiness lost"); return a; }
        if (i->mode == GUIDED) enter(p, PATROL_ARM, now, "requesting normal arm");
        else if (now-p->entered_ms > 5000U) enter(p, PATROL_IDLE, now, "Guided change timed out");
        else if (!p->sent) { a.type = PATROL_SET_MODE; a.mode = GUIDED; p->sent = true; }
        return a;
    }
    if (p->state == PATROL_ARM) {
        if (i->armed) { enter(p, PATROL_TAKEOFF, now, "takeoff to 3m; TFmini verifies height"); return a; }
        if (!i->prearm_ok || !i->on_ground) {
            enter(p, PATROL_IDLE, now, "pre-arm/ground check failed"); return a;
        }
        if (p->sent && i->ack_serial != p->ack_serial && i->ack_command == 400U && i->ack_result != 0U && i->ack_result != 5U) {
            enter(p, PATROL_IDLE, now, "FC rejected arm command"); return a;
        }
        if (now-p->entered_ms > 5000U) { enter(p, PATROL_IDLE, now, "arming timed out"); return a; }
        if (!p->sent) { a.type = PATROL_ARM_CMD; p->sent = true; p->ack_serial = i->ack_serial; }
        return a;
    }
    if (p->state == PATROL_TAKEOFF) {
        if (!p->sent) {
            /* NAV_TAKEOFF is above home; compensate sensor clearance at launch.
             * Require TFmini confirmation before any horizontal movement. */
            a.type = PATROL_TAKEOFF_CMD; a.z = 3.0f-i->height;
            p->sent = true; p->ack_serial = i->ack_serial; return a;
        }
        bool rejected = i->ack_serial != p->ack_serial && i->ack_command == 22U && i->ack_result != 0U && i->ack_result != 5U;
        if (rejected || now-p->entered_ms > 30000U) {
            enter(p, PATROL_LANDING, now, rejected ? "FC rejected takeoff" : "takeoff timed out");
            a.type = PATROL_SET_MODE; a.mode = LAND; return a;
        }
        if (settled(p, !i->on_ground && fabsf(i->height-3.0f) < 0.3f && fabsf(i->vz) < 0.3f, now)) {
            corner(p); enter(p, PATROL_LEG, now, "flying rectangle");
        }
        return a;
    }
    if ((p->state == PATROL_LEG || p->state == PATROL_TURN) && now-p->entered_ms > 60000U) {
        enter(p, PATROL_LANDING, now, "leg/turn timed out");
        a.type = PATROL_SET_MODE; a.mode = LAND; return a;
    }
    bool height_good = fabsf(i->height-3.0f) < 0.35f;
    if (p->state == PATROL_LEG && settled(p,
        hypotf(i->x-p->target_x, i->y-p->target_y) < 0.8f &&
        hypotf(i->vx,i->vy) < 0.3f && height_good, now)) {
        p->target_yaw = wrap(p->initial_yaw + (p->leg+1U)*PI*0.5f);
        enter(p, PATROL_TURN, now, "turning right 90 degrees");
    } else if (p->state == PATROL_TURN && settled(p,
        fabsf(wrap(p->target_yaw-i->yaw)) < PI/18.0f &&
        hypotf(i->vx,i->vy) < 0.3f && height_good, now)) {
        if (++p->leg == 4U) enter(p, PATROL_HOVER, now, "rectangle complete; hovering at 3m");
        else { corner(p); enter(p, PATROL_LEG, now, "flying rectangle"); }
    }
    a.type = PATROL_TARGET;
    /* Short lookahead caps horizontal demand to 0.5m ahead; FC limits actual
     * acceleration/speed. Step the intermediate target at at most 0.5m/s. */
    float dx = p->target_x-p->command_x, dy = p->target_y-p->command_y;
    float dist = hypotf(dx,dy), lookahead = fminf(0.1f, 0.5f*elapsed*0.001f);
    float scale = dist > lookahead && dist > 0 ? lookahead/dist : 1.0f;
    if (hypotf(p->command_x-i->x, p->command_y-i->y) < 0.5f) {
        p->command_x += dx*scale; p->command_y += dy*scale;
    }
    a.x = p->command_x; a.y = p->command_y;
    /* NED z increases down. Tilt-corrected ToF height supplies vertical error;
     * bound target lookahead, rather than changing EKF height-source params. */
    a.z = i->z + clamp(i->height-3.0f, 0.25f);
    a.yaw = wrap(i->yaw + clamp(wrap(p->target_yaw-i->yaw), PI/6.0f*elapsed*0.001f));
    return a;
}
