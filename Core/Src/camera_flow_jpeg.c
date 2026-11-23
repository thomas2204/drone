#include "camera_flow.h"
#include "main.h"
#include <string.h>

/* A chunk is a whole number of minimum coded units for the supported
 * grayscale and YCbCr layouts. No full VGA pixel buffer is required. */
static uint32_t output[3072 / sizeof(uint32_t)];
static JPEG_HandleTypeDef codec;
static uint8_t *gray;
static unsigned unit_bytes, luma_cols, luma_rows, unit_index;
static bool complete, invalid;

void HAL_JPEG_InfoReadyCallback(JPEG_HandleTypeDef *handle, JPEG_ConfTypeDef *info)
{
    if (handle != &codec) return;
    if (info->ImageWidth != 640 || info->ImageHeight != 480) {
        invalid = true;
        return;
    }
    luma_cols = 1;
    luma_rows = 1;
    if (info->ColorSpace == JPEG_GRAYSCALE_COLORSPACE) {
        unit_bytes = 64;
    } else if (info->ColorSpace == JPEG_YCBCR_COLORSPACE) {
        switch (info->ChromaSubsampling) {
        case JPEG_444_SUBSAMPLING: break;
        case JPEG_422_SUBSAMPLING: luma_cols = 2; break;
        case JPEG_420_SUBSAMPLING: luma_cols = luma_rows = 2; break;
        default: invalid = true; return;
        }
        unit_bytes = 64U * (luma_cols * luma_rows + 2U);
    } else {
        invalid = true;
    }
}

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *handle, uint8_t *data, uint32_t bytes)
{
    if (handle != &codec) return;
    if (unit_bytes == 0U || bytes % unit_bytes != 0U) invalid = true;
    if (!invalid) {
        const unsigned columns = 80U / luma_cols;
        const unsigned units = columns * (60U / luma_rows);
        for (uint32_t offset = 0; offset < bytes; offset += unit_bytes) {
            if (unit_index >= units) {
                invalid = true;
                break;
            }
            const unsigned x = (unit_index % columns) * luma_cols;
            const unsigned y = (unit_index / columns) * luma_rows;
            for (unsigned dy = 0; dy < luma_rows; ++dy) {
                for (unsigned dx = 0; dx < luma_cols; ++dx) {
                    const uint8_t *block = data + offset + 64U * (dy * luma_cols + dx);
                    unsigned sum = 0;
                    for (unsigned i = 0; i < 64; ++i) sum += block[i];
                    gray[(y + dy) * 80U + x + dx] = (uint8_t)((sum + 32U) / 64U);
                }
            }
            ++unit_index;
        }
    }
    HAL_JPEG_ConfigOutputBuffer(handle, (uint8_t *)output, sizeof(output));
}

void HAL_JPEG_DecodeCpltCallback(JPEG_HandleTypeDef *handle)
{
    if (handle == &codec) complete = true;
}

void HAL_JPEG_ErrorCallback(JPEG_HandleTypeDef *handle)
{
    if (handle == &codec) invalid = true;
}

void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *handle, uint32_t consumed)
{
    (void)consumed;
    if (handle == &codec) HAL_JPEG_ConfigInputBuffer(handle, NULL, 0);
}

bool CameraFlow_DecodeHardware(uint8_t *jpeg, size_t bytes, uint8_t *pixels)
{
    if (codec.Instance == NULL) {
        __HAL_RCC_JPGDECEN_CLK_ENABLE();
        codec.Instance = JPEG;
        if (HAL_JPEG_Init(&codec) != HAL_OK) {
            memset(&codec, 0, sizeof(codec));
            return false;
        }
    }
    gray = pixels;
    unit_bytes = unit_index = 0;
    complete = invalid = false;
    HAL_StatusTypeDef result = HAL_JPEG_Decode(&codec, jpeg, (uint32_t)bytes,
        (uint8_t *)output, sizeof(output), 180U);
    bool ok = result == HAL_OK && complete && !invalid && unit_bytes != 0U &&
        unit_index == (80U / luma_cols) * (60U / luma_rows);
    if (!ok) {
        SET_BIT(RCC->AHB3RSTR, RCC_AHB3RSTR_JPGDECRST);
        (void)RCC->AHB3RSTR;
        CLEAR_BIT(RCC->AHB3RSTR, RCC_AHB3RSTR_JPGDECRST);
        memset(&codec, 0, sizeof(codec));
    }
    return ok;
}
