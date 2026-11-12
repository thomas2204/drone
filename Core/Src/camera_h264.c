#include "camera_h264.h"
#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#define H264E_ENABLE_DENOISE 0
#define __BYTE_ORDER 0
#define __BIG_ENDIAN 1
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

static uint8_t encoder_memory[160U * 1024U]
    __attribute__((section(".dma_buffer"), aligned(32)));
static uint8_t yuv[320U * 240U * 3U / 2U]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
static int scratch_needed;

static uint8_t Clip(int value) { return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value; }

bool CameraH264_Init(void)
{
    H264E_create_param_t p = {0};
    p.width = 320; p.height = 240; p.gop = 1;
    int persistent;
    if (H264E_sizeof(&p, &persistent, &scratch_needed) ||
        persistent > (int)sizeof(encoder_memory)) return false;
    return H264E_init((H264E_persist_t *)encoder_memory, &p) == 0;
}

bool CameraH264_Encode(uint8_t *bgr, size_t scratch_bytes,
                       const uint8_t **output, uint32_t *bytes)
{
    if (!bgr || !output || !bytes || scratch_needed <= 0 ||
        scratch_bytes < (size_t)scratch_needed) return false;
    /* BT.601 limited-range I420. Average chroma over each 2x2 pixel group. */
    for (unsigned y = 0; y < 240; y += 2) {
        for (unsigned x = 0; x < 320; x += 2) {
            int sum_u = 0, sum_v = 0;
            for (unsigned dy = 0; dy < 2; ++dy) {
                const uint8_t *row = bgr + (239U - y - dy) * 960U;
                for (unsigned dx = 0; dx < 2; ++dx) {
                    const uint8_t *p = row + (x + dx) * 3U;
                    int b = p[0], g = p[1], r = p[2];
                    yuv[(y + dy) * 320U + x + dx] = Clip(((66*r + 129*g + 25*b + 128) >> 8) + 16);
                    sum_u += -38*r - 74*g + 112*b;
                    sum_v += 112*r - 94*g - 18*b;
                }
            }
            unsigned at = (y / 2U) * 160U + x / 2U;
            yuv[76800U + at] = Clip(((sum_u + 512) >> 10) + 128);
            yuv[96000U + at] = Clip(((sum_v + 512) >> 10) + 128);
        }
    }
    H264E_io_yuv_t input = {{yuv, yuv + 76800, yuv + 96000}, {320, 160, 160}};
    H264E_run_param_t run = {0};
    run.encode_speed = H264E_SPEED_FASTEST;
    run.frame_type = H264E_FRAME_TYPE_KEY;
    run.qp_min = 28; run.qp_max = 28;
    unsigned char *encoded = NULL;
    int count = 0;
    int status = H264E_encode((H264E_persist_t *)encoder_memory,
        (H264E_scratch_t *)bgr, &run, &input, &encoded, &count);
    if (status || count <= 0) return false;
    *output = encoded; *bytes = (uint32_t)count;
    return true;
}
