#include "camera_jpeg.h"

/* The caller has already found the start-of-image marker. */
static int ParseJPEG(const uint8_t *data, size_t length, size_t *size,
                     uint16_t *width, uint16_t *height)
{
    uint16_t w = 0, h = 0;
    int scan = 0;
    size_t p = 2;
    while (p < length) {
        if (data[p++] != 0xFF) {
            if (!scan) return 0;
            continue;
        }
        while (p < length && data[p] == 0xFF) ++p;
        if (p >= length) return 0;
        uint8_t marker = data[p++];
        if (marker == 0x00) { if (!scan) return 0; continue; }
        if (marker >= 0xD0 && marker <= 0xD7) { if (!scan) return 0; continue; }
        if (marker == 0xD9) {
            if (!scan || !w || !h) return 0;
            *size = p; *width = w; *height = h;
            return 1;
        }
        if (marker == 0xD8 || marker == 0x01) return 0;
        if (p + 2 > length) return 0;
        size_t segment = ((size_t)data[p] << 8) | data[p + 1];
        if (segment < 2 || segment > length - p) return 0;
        if (marker == 0xC0) {
            if (segment < 11 || data[p + 2] != 8 || !data[p + 7] ||
                segment != (size_t)8 + 3U * data[p + 7]) return 0;
            h = (uint16_t)((data[p + 3] << 8) | data[p + 4]);
            w = (uint16_t)((data[p + 5] << 8) | data[p + 6]);
        }
        if (marker == 0xDA) {
            if (segment < 8 || !data[p + 2] ||
                segment != (size_t)6 + 2U * data[p + 2]) return 0;
            scan = 1;
        }
        p += segment;
    }
    return 0;
}

int CameraJPEG_Find(const uint8_t *data, size_t length, size_t *offset,
                    size_t *size, uint16_t *width, uint16_t *height)
{
    if (!data || !offset || !size || !width || !height) return 0;
    /* Continuous DMA can contain a damaged frame followed by an intact one. */
    for (size_t start = 0; start + 1 < length; ++start) {
        if (data[start] != 0xFF || data[start + 1] != 0xD8) continue;
        if (ParseJPEG(data + start, length - start, size, width, height)) {
            *offset = start;
            return 1;
        }
    }
    return 0;
}
