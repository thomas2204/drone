#ifndef UWB_ANGLE_H
#define UWB_ANGLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

/* User reports approximately 20 mm spacing. Bearing remains disabled until
 * the user captures forward zero with Bluetooth `uwb zero`, or supplies a
 * measured fixed offset and enables UWB_ANGLE_CALIBRATED. */
#define UWB_ANTENNA_SPACING_M 0.020f
#define UWB_PHASE_OFFSET_RAD 0.0f
#define UWB_ANGLE_CALIBRATED false

#define UWB_FILTER_SAMPLES 10U
#define UWB_FILTER_TRIM 2U

/* Ten-sample circular trimmed mean: unwrap around a circular medoid, discard
 * the two extremes on each side, then average the six inliers. */
typedef struct {
    int16_t samples[UWB_FILTER_SAMPLES];
    uint8_t count, next;
    uint32_t last_ms;
} UWB_PhaseFilter;

static inline bool UWB_FilterPhase(UWB_PhaseFilter *f, int16_t raw,
    uint32_t now_ms, int16_t *filtered)
{
    if (f->count && (uint32_t)(now_ms - f->last_ms) > 500U)
        f->count = f->next = 0;
    f->last_ms = now_ms;
    f->samples[f->next] = raw;
    f->next = (uint8_t)((f->next + 1U) % UWB_FILTER_SAMPLES);
    if (f->count < UWB_FILTER_SAMPLES) ++f->count;
    if (f->count < UWB_FILTER_SAMPLES) return false;
    float best = INFINITY;
    unsigned selected = 0;
    for (unsigned i = 0; i < UWB_FILTER_SAMPLES; ++i) {
        float cost = 0;
        for (unsigned j = 0; j < UWB_FILTER_SAMPLES; ++j)
            cost += fabsf(remainderf(((float)f->samples[i] - f->samples[j]) /
                2048.0f, 6.283185307179586f));
        if (cost < best) { best = cost; selected = i; }
    }
    float delta[UWB_FILTER_SAMPLES];
    for (unsigned i = 0; i < UWB_FILTER_SAMPLES; ++i) {
        delta[i] = remainderf(((float)f->samples[i] - f->samples[selected]) /
            2048.0f, 6.283185307179586f);
        for (unsigned j = i; j && delta[j] < delta[j - 1U]; --j) {
            float t = delta[j]; delta[j] = delta[j - 1U]; delta[j - 1U] = t;
        }
    }
    float sum = 0;
    for (unsigned i = UWB_FILTER_TRIM; i < UWB_FILTER_SAMPLES - UWB_FILTER_TRIM; ++i)
        sum += delta[i];
    float phase = (float)f->samples[selected] / 2048.0f +
        sum / (UWB_FILTER_SAMPLES - 2U * UWB_FILTER_TRIM);
    *filtered = (int16_t)lroundf(remainderf(phase, 6.283185307179586f) * 2048.0f);
    return true;
}

typedef struct {
    int32_t samples[UWB_FILTER_SAMPLES];
    uint8_t count, next;
    uint32_t last_ms;
} UWB_DistanceFilter;

static inline bool UWB_FilterDistance(UWB_DistanceFilter *f, int32_t mm,
    uint32_t now_ms, int32_t *filtered)
{
    if (f->count && (uint32_t)(now_ms - f->last_ms) > 500U)
        f->count = f->next = 0;
    f->last_ms = now_ms;
    f->samples[f->next] = mm;
    f->next = (uint8_t)((f->next + 1U) % UWB_FILTER_SAMPLES);
    if (f->count < UWB_FILTER_SAMPLES) ++f->count;
    if (f->count < UWB_FILTER_SAMPLES) return false;
    int32_t sorted[UWB_FILTER_SAMPLES];
    for (unsigned i = 0; i < UWB_FILTER_SAMPLES; ++i) {
        sorted[i] = f->samples[i];
        for (unsigned j = i; j && sorted[j] < sorted[j - 1U]; --j) {
            int32_t t = sorted[j]; sorted[j] = sorted[j - 1U]; sorted[j - 1U] = t;
        }
    }
    int64_t sum = 0;
    for (unsigned i = UWB_FILTER_TRIM; i < UWB_FILTER_SAMPLES - UWB_FILTER_TRIM; ++i)
        sum += sorted[i];
    *filtered = (int32_t)(sum / (UWB_FILTER_SAMPLES - 2U * UWB_FILTER_TRIM));
    return true;
}

/* Decode the driver's full signed 41-bit TDoA; do not truncate to 16 bits. */
static inline int64_t UWB_DecodeTdoa(const uint8_t bytes[6])
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 6; ++i) value |= (uint64_t)bytes[i] << (8U * i);
    value &= (UINT64_C(1) << 41) - 1;
    return (value & (UINT64_C(1) << 40)) ?
        (int64_t)value - (INT64_C(1) << 41) : (int64_t)value;
}

/* Ideal far-field, front-sector bearing from broadside, in millidegrees.
 * Only valid for a known tracking plane; one baseline cannot resolve 3D/front-back.
 * Reject impossible geometry instead of clamping it into a plausible angle. */
static inline bool UWB_PhaseToBearing(int16_t raw, float spacing_m,
    float offset_rad, uint8_t channel, int32_t *bearing_mdeg)
{
    const float pi = 3.14159265358979323846f;
    float frequency = channel == 5 ? 6489600000.0f : 7987200000.0f;
    if (bearing_mdeg == NULL || (channel != 5 && channel != 9) ||
        !isfinite(spacing_m) || !isfinite(offset_rad) || spacing_m <= 0.0f)
        return false;
    float wavelength = 299792458.0f / frequency;
    if (spacing_m > wavelength * 0.5f) return false;
    float phase = remainderf((float)raw / 2048.0f - offset_rad, 2.0f * pi);
    float sine = phase * wavelength / (2.0f * pi * spacing_m);
    if (!isfinite(sine) || fabsf(sine) > 1.0f) return false;
    *bearing_mdeg = (int32_t)lroundf(asinf(sine) * (180000.0f / pi));
    return true;
}

#endif
