#ifndef FC_ARM_STATUS_H
#define FC_ARM_STATUS_H

#include "mk_app.h"

typedef enum
{
    FC_ARM_DISCONNECTED,
    FC_ARM_ALREADY_ARMED,
    FC_ARM_STATUS_STALE,
    FC_ARM_CHECKS_UNAVAILABLE,
    FC_ARM_NOT_READY,
    FC_ARM_READY
} FC_ArmStatus;

/* Use the FC's reported pre-arm result for both Bluetooth and the strip. */
static inline FC_ArmStatus FC_GetArmStatus(const FC_Telemetry_t *fc, uint32_t now)
{
    if (!fc->connected) return FC_ARM_DISCONNECTED;
    if (fc->armed) return FC_ARM_ALREADY_ARMED;
    if (fc->sys_status_count == 0U ||
        (uint32_t)(now - fc->last_sys_status_ms) >= 3000U)
        return FC_ARM_STATUS_STALE;
    if (!fc->prearm_supported || !fc->prearm_enabled)
        return FC_ARM_CHECKS_UNAVAILABLE;
    return fc->prearm_healthy ? FC_ARM_READY : FC_ARM_NOT_READY;
}

static inline const char *FC_ArmReadiness(const FC_Telemetry_t *fc, uint32_t now)
{
    switch (FC_GetArmStatus(fc, now)) {
    case FC_ARM_DISCONNECTED: return "UNKNOWN (FC disconnected)";
    case FC_ARM_ALREADY_ARMED: return "ALREADY ARMED";
    case FC_ARM_STATUS_STALE: return "UNKNOWN (pre-arm status unavailable/stale)";
    case FC_ARM_CHECKS_UNAVAILABLE: return "UNKNOWN (FC pre-arm checks not reported/enabled)";
    case FC_ARM_NOT_READY: return "NOT READY (FC pre-arm checks failing)";
    case FC_ARM_READY: return "READY TO ARM (FC pre-arm checks passed)";
    }
    return "UNKNOWN (pre-arm status unavailable/stale)";
}

#endif
