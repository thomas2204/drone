#include "fc_arm_status.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    FC_Telemetry_t fc = {0};
    assert(strstr(FC_ArmReadiness(&fc, 100), "UNKNOWN"));
    fc.connected = true;
    assert(strstr(FC_ArmReadiness(&fc, 100), "UNKNOWN"));
    fc.sys_status_count = 1;
    fc.last_sys_status_ms = 100;
    fc.prearm_supported = fc.prearm_enabled = true;
    assert(strstr(FC_ArmReadiness(&fc, 100), "NOT READY"));
    fc.prearm_healthy = true;
    assert(strstr(FC_ArmReadiness(&fc, 3099), "READY TO ARM"));
    assert(strstr(FC_ArmReadiness(&fc, 3100), "UNKNOWN"));
    fc.prearm_enabled = false;
    assert(strstr(FC_ArmReadiness(&fc, 100), "UNKNOWN"));
    fc.prearm_enabled = true;
    fc.prearm_supported = false;
    assert(strstr(FC_ArmReadiness(&fc, 100), "UNKNOWN"));
    fc.armed = true;
    assert(strcmp(FC_ArmReadiness(&fc, 100), "ALREADY ARMED") == 0);
    fc.connected = false;
    assert(strstr(FC_ArmReadiness(&fc, 100), "UNKNOWN"));
    fc.connected = fc.prearm_supported = true;
    fc.armed = false;
    fc.last_sys_status_ms = UINT32_MAX - 100U;
    assert(strstr(FC_ArmReadiness(&fc, 100), "READY TO ARM"));
    return 0;
}
