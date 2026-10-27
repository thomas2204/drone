#ifndef GPS_INPUT_H
#define GPS_INPUT_H

#include "gps_raw.h"
#include "common/mavlink.h"

// Pure conversion. False means samples are stale, incomplete or not synchronized.
// True may return fix_type=1 for an explicitly reported loss of fix.
bool GPS_BuildInput(const GPS_RawSnapshot_t *gps, uint32_t now_ms,
                    mavlink_gps_input_t *out);

#endif
