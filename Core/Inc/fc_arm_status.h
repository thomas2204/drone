#ifndef FC_ARM_STATUS_H
#define FC_ARM_STATUS_H

#include "mk_app.h"

/* Report the FC's pre-arm result, never infer readiness from GPS alone. */
static inline const char *FC_ArmReadiness(const FC_Telemetry_t *fc, uint32_t now)
{
    if (!fc->connected) return "UNKNOWN (FC disconnected)";
    if (fc->armed) return "ALREADY ARMED";
    if (fc->sys_status_count == 0U ||
        (uint32_t)(now - fc->last_sys_status_ms) >= 3000U)
        return "UNKNOWN (pre-arm status unavailable/stale)";
    if (!fc->prearm_supported || !fc->prearm_enabled)
        return "UNKNOWN (FC pre-arm checks not reported/enabled)";
    return fc->prearm_healthy ? "READY TO ARM (FC pre-arm checks passed)" :
                               "NOT READY (FC pre-arm checks failing)";
}

#endif
