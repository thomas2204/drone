#include "camera_flow.h"
#include "camera_bgr.h"
#include "app_log.h"
#include "main.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "camera_avi.h"
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 80, HEIGHT = 60, PATCH = 7, SEARCH = 6, POINTS = 36 };
static uint8_t images[2][WIDTH * HEIGHT];
/* Largest frame in the bench recordings was 23,720 bytes. Larger frames
 * still go to SD. Only the optional flow analysis skips them. */
static _Alignas(4) uint8_t pending_jpeg[28U * 1024U];
static atomic_bool pending;
static osThreadId_t worker;
static StaticTask_t worker_control;
static uint64_t worker_stack[3072U / sizeof(uint64_t)];

typedef struct {
    uint32_t session, saved, skipped_busy, skipped_size, previous_ms, max_gap;
    uint32_t capture_ms;
    size_t bytes;
#if CAMERA_FLOW_MAVLINK
    bool telemetry_ready;
#endif
} FlowFrame;
static FlowFrame recording, queued;
typedef struct {
    CameraFlowReference fc;
    uint32_t capture_ms;
    uint16_t interval_ms;
    int16_t dx100, dy100, rotation_mrad;
    uint8_t processed, quality;
} FlowTraceRow;
static FlowTraceRow trace[CAMERA_AVI_MAX_FRAMES];
static uint32_t trace_session;
/* Used only by the recorder. The largest gap's parts refer to the same
 * pair of frames, so they add up to the measured gap. */
static struct {
    uint32_t previous_write_end, previous_write;
    uint32_t capture_max, write_max, between_max;
    uint32_t gap_capture, gap_write, gap_between, gap_frame;
} capture_timing;
static struct {
    unsigned current, samples, failures, valid, matches;
    unsigned first_frames, rejected_gap, rejected_patches, rejected_agreement, brightness_recovered;
    float dx, dy, rotation;
    uint8_t quality;
    uint32_t previous_ms, max_gap, max_decode, max_track, last_report, late_gaps;
    bool have_previous;
} flow;

#if CAMERA_FLOW_MAVLINK
static CameraFlowReading reading;

static void PublishFlow(uint32_t capture_ms, uint32_t elapsed, uint8_t quality)
{
    /* Down-mounted camera, image top towards the nose. Angular image
     * motion is around body X for rightward image shift and around body Y
     * for downward shift. ArduPilot applies the gyro compensation. */
    float x = 0, y = 0;
    if (quality && elapsed) {
        const float seconds = elapsed * 0.001f;
        x = atan2f(flow.dx, CAMERA_FLOW_FX_X100 * 0.01f) / seconds;
        y = atan2f(flow.dy, CAMERA_FLOW_FY_X100 * 0.01f) / seconds;
    }
    taskENTER_CRITICAL();
    reading = (CameraFlowReading){reading.sequence + 1U, capture_ms, x, y, quality};
    taskEXIT_CRITICAL();
}

bool CameraFlow_Read(CameraFlowReading *out)
{
    taskENTER_CRITICAL();
    *out = reading;
    taskEXIT_CRITICAL();
    return out->sequence != 0U && out->quality != 0U &&
        (uint32_t)(HAL_GetTick() - out->capture_ms) < 200U;
}
#endif

static void ResetTracking(uint32_t now)
{
#if CAMERA_FLOW_MAVLINK
    /* Recording starts a fresh trace while the live tracker keeps its images. */
    const unsigned current = flow.current;
    const uint32_t previous_ms = flow.previous_ms;
    const bool have_previous = flow.have_previous;
#endif
    memset(&flow, 0, sizeof(flow));
#if CAMERA_FLOW_MAVLINK
    flow.current = current;
    flow.previous_ms = previous_ms;
    flow.have_previous = have_previous;
#endif
    flow.last_report = now;
}

typedef struct { float x, y, dx, dy; } Match;
typedef struct { float dx, dy, scale, turn; } Motion;

static float Residual(const Motion *m, const Match *p)
{
    float x = p->dx - (m->dx + m->scale * p->x - m->turn * p->y);
    float y = p->dy - (m->dy + m->turn * p->x + m->scale * p->y);
    return x*x + y*y;
}

/* Fit a single planar similarity motion. At image centre the rotation term
 * vanishes, so yaw doesn't have to look like the same shift at every tile. */
static bool Fit(const Match *points, unsigned count, Motion *result)
{
    const float threshold = 2.25f;
    Motion best = {0};
    unsigned best_count = 0;
    float best_error = 0;
    for (unsigned a = 0; a < count; ++a) {
        for (unsigned b = a + 1; b < count; ++b) {
            const Match *p = &points[a], *q = &points[b];
            float x = q->x - p->x, y = q->y - p->y;
            float separation = x*x + y*y;
            if (separation < 256.0f) continue;
            float u = q->dx - p->dx, v = q->dy - p->dy;
            Motion m = { .scale = (x*u + y*v)/separation,
                         .turn = (x*v - y*u)/separation };
            if (fabsf(m.scale) > 0.12f || fabsf(m.turn) > 0.35f) continue;
            m.dx = p->dx - m.scale*p->x + m.turn*p->y;
            m.dy = p->dy - m.turn*p->x - m.scale*p->y;
            unsigned inliers = 0;
            float error = 0;
            for (unsigned i = 0; i < count; ++i) {
                float residual = Residual(&m, &points[i]);
                if (residual > threshold) continue;
                ++inliers;
                error += residual;
            }
            if (inliers > best_count || (inliers == best_count && error < best_error)) {
                best = m;
                best_count = inliers;
                best_error = error;
            }
        }
    }
    flow.matches = best_count;
    if (best_count < 5 || best_count * 2U < count) return false;

    /* Refine using all inliers and require coverage on both sides of the
     * centre on both axes before trusting the centre displacement. */
    float mx = 0, my = 0, mu = 0, mv = 0;
    unsigned sides = 0;
    for (unsigned i = 0; i < count; ++i) {
        const Match *p = &points[i];
        if (Residual(&best, p) > threshold) continue;
        mx += p->x; my += p->y; mu += p->dx; mv += p->dy;
        if (p->x < -7) sides |= 1U;
        if (p->x > 7) sides |= 2U;
        if (p->y < -7) sides |= 4U;
        if (p->y > 7) sides |= 8U;
    }
    if (sides != 15U) return false;
    const float reciprocal = 1.0f / (float)best_count;
    mx *= reciprocal; my *= reciprocal; mu *= reciprocal; mv *= reciprocal;
    float variance = 0, scale = 0, turn = 0;
    for (unsigned i = 0; i < count; ++i) {
        const Match *p = &points[i];
        if (Residual(&best, p) > threshold) continue;
        float x = p->x - mx, y = p->y - my, u = p->dx - mu, v = p->dy - mv;
        variance += x*x + y*y;
        scale += x*u + y*v;
        turn += x*v - y*u;
    }
    if (variance < 256.0f) return false;
    result->scale = scale / variance;
    result->turn = turn / variance;
    result->dx = mu - result->scale*mx + result->turn*my;
    result->dy = mv - result->turn*mx - result->scale*my;
    return fabsf(result->scale) <= 0.12f && fabsf(result->turn) <= 0.35f;
}

static unsigned PatchError(const uint8_t *previous, const uint8_t *current,
                           int x, int y, int dx, int dy, unsigned cutoff, bool compensate)
{
    int offset = 0;
    if (compensate) {
        /* Estimate a local exposure change from the four patch corners.
         * This leaves the patch's shape available for matching. */
        const uint8_t *a = previous + y*WIDTH + x;
        const uint8_t *b = current + (y + dy)*WIDTH + x + dx;
        const int bottom = (PATCH - 1)*WIDTH;
        offset = ((a[0] - b[0]) + (a[PATCH - 1] - b[PATCH - 1]) +
                  (a[bottom] - b[bottom]) +
                  (a[bottom + PATCH - 1] - b[bottom + PATCH - 1])) / 4;
    }
    unsigned error = 0;
    for (int row = 0; row < PATCH; ++row) {
        const uint8_t *a = previous + (y + row)*WIDTH + x;
        const uint8_t *b = current + (y + row + dy)*WIDTH + x + dx;
        for (int column = 0; column < PATCH; ++column)
            error += (unsigned)abs(a[column] - b[column] - offset);
        if (error >= cutoff) break;
    }
    return error;
}

static float Subpixel(unsigned left, unsigned middle, unsigned right)
{
    int curve = (int)left + (int)right - 2*(int)middle;
    return curve > 0 ? 0.5f * ((int)left - (int)right) / curve : 0.0f;
}

/* Prefer corners over straight grout edges within each grid cell. Edges
 * alone can slide along their length and produce ambiguous matches. */
static unsigned CornerScore(const uint8_t *image, int x, int y)
{
    int xx = 0, xy = 0, yy = 0;
    for (int row = 1; row < PATCH - 1; ++row) {
        const uint8_t *p = image + (y + row)*WIDTH + x;
        for (int column = 1; column < PATCH - 1; ++column) {
            int dx = (p[column + 1] - p[column - 1]) / 4;
            int dy = (p[column + WIDTH] - p[column - WIDTH]) / 4;
            xx += dx*dx; xy += dx*dy; yy += dy*dy;
        }
    }
    int trace = xx + yy;
    return trace ? (unsigned)(((int64_t)xx*yy - (int64_t)xy*xy) / trace) : 0;
}

typedef enum { TRACK_VALID, TRACK_TOO_FEW, TRACK_DISAGREEMENT } TrackResult;

static TrackResult TrackPass(const uint8_t *previous, const uint8_t *current, bool compensate)
{
    Match points[POINTS];
    unsigned count = 0;
    for (int row = 8; row <= 43; row += 7) {
        for (int column = 8; column <= 63; column += 11) {
            unsigned corner = 0;
            int x = column, y = row;
            for (int oy = -2; oy <= 2; oy += 2) {
                for (int ox = -2; ox <= 2; ox += 2) {
                    unsigned score = CornerScore(previous, column + ox, row + oy);
                    if (score <= corner) continue;
                    corner = score;
                    x = column + ox;
                    y = row + oy;
                }
            }
            if (corner < 12) continue;
            unsigned best = UINT_MAX, second = UINT_MAX;
            int best_x = 0, best_y = 0;
            int low = 255, high = 0;
            for (int py = 0; py < PATCH; ++py) {
                for (int px = 0; px < PATCH; ++px) {
                    int value = previous[(y + py) * WIDTH + x + px];
                    if (value < low) low = value;
                    if (value > high) high = value;
                }
            }
            if (high - low < 20) continue;
            for (int dy = -SEARCH; dy <= SEARCH; ++dy) {
                for (int dx = -SEARCH; dx <= SEARCH; ++dx) {
                    unsigned error = PatchError(previous, current, x, y, dx, dy, second, compensate);
                    if (error < best) {
                        second = best;
                        best = error;
                        best_x = dx;
                        best_y = dy;
                    } else if (error < second) {
                        second = error;
                    }
                }
            }
            if (best > 16U * PATCH * PATCH || second - best < 20 ||
                abs(best_x) == SEARCH || abs(best_y) == SEARCH) continue;
            float fx = Subpixel(PatchError(previous,current,x,y,best_x-1,best_y,UINT_MAX,compensate), best,
                                PatchError(previous,current,x,y,best_x+1,best_y,UINT_MAX,compensate));
            float fy = Subpixel(PatchError(previous,current,x,y,best_x,best_y-1,UINT_MAX,compensate), best,
                                PatchError(previous,current,x,y,best_x,best_y+1,UINT_MAX,compensate));
            points[count] = (Match){ x + 3.0f - 39.5f, y + 3.0f - 29.5f,
                                     best_x + fx, best_y + fy };
            ++count;
        }
    }
    flow.matches = 0;
    flow.dx = 0;
    flow.dy = 0;
    if (count < 5) return TRACK_TOO_FEW;
    Motion motion;
    if (!Fit(points, count, &motion)) return TRACK_DISAGREEMENT;
    flow.dx = motion.dx;
    flow.dy = motion.dy;
    flow.rotation = atan2f(motion.turn, 1.0f + motion.scale);
    flow.quality = (uint8_t)(255U * flow.matches / POINTS);
    return TRACK_VALID;
}

static void Track(const uint8_t *previous, const uint8_t *current)
{
    TrackResult result = TrackPass(previous, current, false);
    if (result != TRACK_VALID) {
        result = TrackPass(previous, current, true);
        if (result == TRACK_VALID) ++flow.brightness_recovered;
    }
    if (result == TRACK_VALID) ++flow.valid;
    else if (result == TRACK_TOO_FEW) ++flow.rejected_patches;
    else ++flow.rejected_agreement;
}

static void Sample(uint8_t *jpeg, size_t bytes, uint32_t capture_ms)
{
    uint32_t start = HAL_GetTick();
    ++flow.samples;
    flow.matches = 0;
    flow.dx = 0;
    flow.dy = 0;
    flow.rotation = 0;
    flow.quality = 0;
    if (flow.have_previous) {
        uint32_t gap = capture_ms - flow.previous_ms;
        if (gap > flow.max_gap) flow.max_gap = gap;
        if (gap >= 100U) ++flow.late_gaps;
    }
#if CAMERA_FLOW_HW_JPEG
    bool decoded = CameraFlow_DecodeHardware(jpeg, bytes, images[flow.current]);
#else
    bool decoded = CameraJPEG_DecodeFlowGray(jpeg, bytes, images[flow.current]);
#endif
    uint32_t decoded_ms = HAL_GetTick();
    if (decoded_ms - start > flow.max_decode) flow.max_decode = decoded_ms - start;
    if (decoded) {
        if (!flow.have_previous) {
            ++flow.first_frames;
        } else if (capture_ms == flow.previous_ms || capture_ms - flow.previous_ms >= 200U) {
            ++flow.rejected_gap;
        } else {
            Track(images[flow.current ^ 1U], images[flow.current]);
            uint32_t track_ms = HAL_GetTick() - decoded_ms;
            if (track_ms > flow.max_track) flow.max_track = track_ms;
        }
        flow.current ^= 1U;
        flow.have_previous = true;
        flow.previous_ms = capture_ms;
    } else {
        ++flow.failures;
        flow.have_previous = false;
    }
}

static void Report(const FlowFrame *frame)
{
    uint32_t now = HAL_GetTick();
    if (now - flow.last_report < 1000U) return;
    flow.last_report = now;
    char line[350];
    int size = snprintf(line, sizeof(line),
        "FLOW 80x60 jpeg=%s dx100=%d dy100=%d rotMrad=%d quality=%u matches=%u valid=%u/%u lightFix=%u saved=%lu busy=%lu oversize=%lu decodeErr=%u first=%u rejectGap=%u rejectPatches=%u rejectAgree=%u recGap=%lums flowGap=%lums gaps100=%lu decodeMax=%lums trackMax=%lums\r\n",
#if CAMERA_FLOW_HW_JPEG
        "hw",
#else
        "sw",
#endif
        (int)(flow.dx*100), (int)(flow.dy*100), (int)(flow.rotation*1000),
        (unsigned)flow.quality, flow.matches, flow.valid, flow.samples, flow.brightness_recovered,
        (unsigned long)frame->saved,
        (unsigned long)frame->skipped_busy, (unsigned long)frame->skipped_size, flow.failures,
        flow.first_frames, flow.rejected_gap, flow.rejected_patches, flow.rejected_agreement,
        (unsigned long)frame->max_gap, (unsigned long)flow.max_gap, (unsigned long)flow.late_gaps,
        (unsigned long)flow.max_decode, (unsigned long)flow.max_track);
    if (size > 0 && (size_t)size < sizeof(line))
        AppLog_Write(line, (uint16_t)size, 40);
}

static void Worker(void *arg)
{
    (void)arg;
    uint32_t session = 0;
    for (;;) {
        osThreadFlagsWait(1U, osFlagsWaitAny, osWaitForever);
        if (!atomic_load_explicit(&pending, memory_order_acquire)) continue;
        FlowFrame frame = queued;
        if (session != frame.session) {
            session = frame.session;
            ResetTracking(frame.capture_ms);
        }
        const uint32_t interval = flow.have_previous ? frame.capture_ms - flow.previous_ms : 0U;
        Sample(pending_jpeg, frame.bytes, frame.capture_ms);
        taskENTER_CRITICAL();
        if (trace_session == frame.session && frame.saved && frame.saved <= CAMERA_AVI_MAX_FRAMES) {
            FlowTraceRow *row = &trace[frame.saved - 1U];
            row->interval_ms = interval < UINT16_MAX ? (uint16_t)interval : UINT16_MAX;
            row->dx100 = (int16_t)(flow.dx * 100.0f);
            row->dy100 = (int16_t)(flow.dy * 100.0f);
            row->rotation_mrad = (int16_t)(flow.rotation * 1000.0f);
            row->processed = 1U;
            row->quality = flow.quality;
        }
        taskEXIT_CRITICAL();
#if CAMERA_FLOW_MAVLINK
        PublishFlow(frame.capture_ms, interval, frame.telemetry_ready ? flow.quality : 0U);
#endif
        atomic_store_explicit(&pending, false, memory_order_release);
        if (frame.saved) Report(&frame);
    }
}

bool CameraFlow_Init(void)
{
    const osThreadAttr_t attributes = {
        .name = "camera_flow", .stack_mem = worker_stack, .stack_size = sizeof(worker_stack),
        .cb_mem = &worker_control, .cb_size = sizeof(worker_control),
        .priority = (osPriority_t)(osPriorityLow - 1)
    };
    worker = osThreadNew(Worker, NULL, &attributes);
    return worker != NULL;
}

void CameraFlow_Begin(void)
{
    recording = (FlowFrame){.session = recording.session + 1U};
    taskENTER_CRITICAL();
    trace_session = recording.session;
    taskEXIT_CRITICAL();
    memset(&capture_timing, 0, sizeof(capture_timing));
}

static void QueueFrame(FlowFrame *frame, const uint8_t *jpeg, size_t bytes,
                       uint32_t capture_ms, const CameraFlowReference *reference)
{
    if (bytes > sizeof(pending_jpeg)) {
        ++frame->skipped_size;
        return;
    }
    if (atomic_load_explicit(&pending, memory_order_acquire)) {
        ++frame->skipped_busy;
        return;
    }
    memcpy(pending_jpeg, jpeg, bytes);
#if CAMERA_FLOW_HW_JPEG
    /* The peripheral takes complete words. Pad after EOI within the slot. */
    const size_t padded = (bytes + 3U) & ~(size_t)3U;
    memset(pending_jpeg + bytes, 0, padded - bytes);
    bytes = padded;
#endif
    queued = *frame;
    queued.capture_ms = capture_ms;
    queued.bytes = bytes;
#if CAMERA_FLOW_MAVLINK
    queued.telemetry_ready = reference->imu_age_ms < 150U &&
        reference->tof_age_ms < 200U && reference->tof_mm > 0U;
#else
    (void)reference;
#endif
    atomic_store_explicit(&pending, true, memory_order_release);
    osThreadFlagsSet(worker, 1U);
}

#if CAMERA_FLOW_MAVLINK
void CameraFlow_Stream(const uint8_t *jpeg, size_t bytes, uint32_t capture_ms,
                       const CameraFlowReference *reference)
{
    FlowFrame frame = {.session = recording.session};
    QueueFrame(&frame, jpeg, bytes, capture_ms, reference);
}
#endif

void CameraFlow_Offer(const uint8_t *jpeg, size_t bytes, uint32_t frame_start_ms,
                      uint32_t capture_ms, uint32_t write_end_ms,
                      const CameraFlowReference *reference)
{
    const uint32_t capture = capture_ms - frame_start_ms;
    const uint32_t write = write_end_ms - capture_ms;
    if (capture > capture_timing.capture_max) capture_timing.capture_max = capture;
    if (write > capture_timing.write_max) capture_timing.write_max = write;
    if (recording.saved != 0U) {
        const uint32_t gap = capture_ms - recording.previous_ms;
        const uint32_t between = frame_start_ms - capture_timing.previous_write_end;
        if (between > capture_timing.between_max) capture_timing.between_max = between;
        if (gap > recording.max_gap) {
            recording.max_gap = gap;
            capture_timing.gap_capture = capture;
            capture_timing.gap_write = capture_timing.previous_write;
            capture_timing.gap_between = between;
            capture_timing.gap_frame = recording.saved + 1U;
        }
    }
    capture_timing.previous_write_end = write_end_ms;
    capture_timing.previous_write = write;
    recording.previous_ms = capture_ms;
    ++recording.saved;
    if (recording.saved <= CAMERA_AVI_MAX_FRAMES)
        trace[recording.saved - 1U] = (FlowTraceRow){.fc = *reference, .capture_ms = capture_ms};
    QueueFrame(&recording, jpeg, bytes, capture_ms, reference);
}

bool CameraFlow_WriteTrace(CameraFlowTraceWrite write)
{
    if (!recording.saved || recording.saved > CAMERA_AVI_MAX_FRAMES) return false;
    const uint32_t start = HAL_GetTick();
    while (atomic_load_explicit(&pending, memory_order_acquire) &&
           HAL_GetTick() - start < 250U) osDelay(1);

    const char header[] = "frame,t_ms,flow_dt_ms,processed,quality,dx100,dy100,rot_mrad,"
        "roll_mrad,pitch_mrad,yaw_mrad,gx_mrad_s,gy_mrad_s,gz_mrad_s,imu_age_ms,tof_mm,tof_age_ms"
#if CAMERA_FLOW_MAVLINK
        ",flow_tx_count,fc_flow_rx_count,fc_flow_age_ms,fc_flow_x_mrad_s,fc_flow_y_mrad_s,fc_flow_quality"
#endif
        "\r\n";
    if (!write(header, sizeof(header) - 1U)) return false;
    const uint32_t origin = trace[0].capture_ms;
    for (unsigned i = 0; i < recording.saved; ++i) {
        taskENTER_CRITICAL();
        const FlowTraceRow row = trace[i];
        taskEXIT_CRITICAL();
        char line[208];
        const int length = snprintf(line, sizeof(line),
            "%u,%lu,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u"
#if CAMERA_FLOW_MAVLINK
            ",%lu,%lu,%u,%d,%d,%u"
#endif
            "\r\n",
            i + 1U, (unsigned long)(row.capture_ms - origin), (unsigned)row.interval_ms,
            (unsigned)row.processed, (unsigned)row.quality, row.dx100, row.dy100, row.rotation_mrad,
            row.fc.roll_mrad, row.fc.pitch_mrad, row.fc.yaw_mrad,
            row.fc.gx_mrad_s, row.fc.gy_mrad_s, row.fc.gz_mrad_s,
            (unsigned)row.fc.imu_age_ms, (unsigned)row.fc.tof_mm, (unsigned)row.fc.tof_age_ms
#if CAMERA_FLOW_MAVLINK
            , (unsigned long)row.fc.flow_tx_count, (unsigned long)row.fc.fc_flow_rx_count,
            (unsigned)row.fc.fc_flow_age_ms, row.fc.fc_flow_x_mrad_s, row.fc.fc_flow_y_mrad_s,
            (unsigned)row.fc.fc_flow_quality
#endif
        );
        if (length < 0 || (size_t)length >= sizeof(line) || !write(line, (size_t)length)) return false;
    }
    return true;
}

void CameraFlow_ReportCapture(void)
{
    if (recording.saved == 0U) return;
    char line[230];
    const int size = snprintf(line, sizeof(line),
        "FLOW TIMING saved=%lu recGap=%lums frame=%lu gapCapture=%lums gapWrite=%lums gapBetween=%lums captureMax=%lums writeMax=%lums betweenMax=%lums\r\n",
        (unsigned long)recording.saved, (unsigned long)recording.max_gap,
        (unsigned long)capture_timing.gap_frame, (unsigned long)capture_timing.gap_capture,
        (unsigned long)capture_timing.gap_write, (unsigned long)capture_timing.gap_between,
        (unsigned long)capture_timing.capture_max, (unsigned long)capture_timing.write_max,
        (unsigned long)capture_timing.between_max);
    if (size > 0 && (size_t)size < sizeof(line)) AppLog_Write(line, (uint16_t)size, 40);
}
