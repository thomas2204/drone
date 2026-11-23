#ifndef CAMERA_FLOW_H
#define CAMERA_FLOW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool CameraFlow_Init(void);
void CameraFlow_Begin(void);
typedef struct {
    uint16_t imu_age_ms, tof_age_ms, tof_mm;
    int16_t roll_mrad, pitch_mrad, yaw_mrad;
    int16_t gx_mrad_s, gy_mrad_s, gz_mrad_s;
#if CAMERA_FLOW_MAVLINK
    uint32_t flow_tx_count, fc_flow_rx_count;
    uint16_t fc_flow_age_ms;
    int16_t fc_flow_x_mrad_s, fc_flow_y_mrad_s;
    uint8_t fc_flow_quality;
#endif
} CameraFlowReference;
/* Uses the most recent FC telemetry at capture completion, with its age. */
void MAVLink_GetFlowReference(CameraFlowReference *reference);
void CameraFlow_Offer(const uint8_t *jpeg, size_t bytes, uint32_t frame_start_ms,
                      uint32_t capture_ms, uint32_t write_end_ms,
                      const CameraFlowReference *reference);
void CameraFlow_ReportCapture(void);
typedef bool (*CameraFlowTraceWrite)(const void *data, size_t bytes);
bool CameraFlow_WriteTrace(CameraFlowTraceWrite write);
#if CAMERA_FLOW_MAVLINK
void CameraFlow_Stream(const uint8_t *jpeg, size_t bytes, uint32_t capture_ms,
                       const CameraFlowReference *reference);
typedef struct {
    uint32_t sequence, capture_ms;
    float rate_x, rate_y;
    uint8_t quality;
} CameraFlowReading;
bool CameraFlow_Read(CameraFlowReading *reading);
#endif
#if CAMERA_FLOW_HW_JPEG
bool CameraFlow_DecodeHardware(uint8_t *jpeg, size_t bytes, uint8_t *pixels);
#endif
#endif
