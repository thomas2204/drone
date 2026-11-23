#include "camera_bgr.h"
#include "tjpgd.h"
#include <string.h>

typedef struct {
    const uint8_t *jpeg;
    size_t bytes, position;
    uint8_t *pixels;
    uint16_t width, height;
    uint32_t stride;
} DecodeContext;

static uint32_t workspace[2048];

static size_t Input(JDEC *decoder, uint8_t *buffer, size_t count)
{
    DecodeContext *c = decoder->device;
    if (count > c->bytes - c->position) count = c->bytes - c->position;
    if (buffer) memcpy(buffer, c->jpeg + c->position, count);
    c->position += count;
    return count;
}

static int Output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    DecodeContext *c = decoder->device;
    if (rect->right >= c->width || rect->bottom >= c->height) return 0;
    const uint8_t *rgb = bitmap;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        uint8_t *bgr = c->pixels + (c->height - 1U - y) * c->stride + rect->left * 3U;
        for (unsigned x = rect->left; x <= rect->right; ++x) {
            bgr[0] = rgb[2]; bgr[1] = rgb[1]; bgr[2] = rgb[0];
            rgb += 3; bgr += 3;
        }
    }
    return 1;
}

bool CameraBGR_Decode(const uint8_t *jpeg, size_t bytes, uint8_t *pixels,
                      uint16_t width, uint16_t height)
{
    if (!jpeg || !pixels || !bytes || !width || !height) return false;
    DecodeContext c = {jpeg, bytes, 0, pixels, width, height, (width * 3U + 3U) & ~3U};
    JDEC decoder;
    if (jd_prepare(&decoder, Input, workspace, sizeof(workspace), &c) != JDR_OK ||
        decoder.width != width || decoder.height != height) return false;
    memset(pixels, 0, c.stride * height);
    return jd_decomp(&decoder, Output, 0) == JDR_OK;
}

static int OutputGray(JDEC *decoder, void *bitmap, JRECT *rect)
{
    DecodeContext *c = decoder->device;
    if (rect->right >= c->width || rect->bottom >= c->height) return 0;
    const uint8_t *rgb = bitmap;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        uint8_t *pixel = c->pixels + y * c->stride + rect->left;
        for (unsigned x = rect->left; x <= rect->right; ++x) {
            *pixel++ = (uint8_t)((77U * rgb[0] + 150U * rgb[1] + 29U * rgb[2]) >> 8);
            rgb += 3;
        }
    }
    return 1;
}

bool CameraJPEG_DecodeFlowGray(const uint8_t *jpeg, size_t bytes, uint8_t *pixels)
{
    /* The flow task can decode while the recorder uses the BGR decoder. */
    static uint32_t flow_workspace[2048];
    if (!jpeg || !pixels || !bytes) return false;
    DecodeContext c = {jpeg, bytes, 0, pixels, 80, 60, 80};
    JDEC decoder;
    return jd_prepare(&decoder, Input, flow_workspace, sizeof(flow_workspace), &c) == JDR_OK &&
        decoder.width == 640 && decoder.height == 480 &&
        jd_decomp(&decoder, OutputGray, 3) == JDR_OK;
}
