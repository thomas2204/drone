#include "audio_player.h"
#include "sai.h"
#include "cmsis_os2.h"
#include <stdbool.h>
#include <string.h>

#define HALF_FRAMES 1024U
#define HALF_SAMPLES (HALF_FRAMES * 2U)
#define FLAG_HALF 1U
#define FLAG_FULL 2U
#define FLAG_ERROR 4U
#define AUDIO_FLAGS (FLAG_HALF | FLAG_FULL | FLAG_ERROR)

/* Both SDMMC1 IDMA and SAI DMA1 can access AXI SRAM. */
static int16_t output[HALF_SAMPLES * 2U]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
static int16_t input[HALF_SAMPLES]
    __attribute__((section(".sd_dma_buffer"), aligned(32)));
static osThreadId_t volatile playback_thread;
static uint32_t remaining;
static uint32_t playback_volume_percent;
static uint32_t half_frames[2];
static bool last_half[2];
static bool idle_silence_running;

volatile AudioPlayerState audio_state = AUDIO_IDLE;
volatile FRESULT audio_file_result = FR_OK;   //simply debug shit
volatile uint32_t audio_sai_error;
volatile uint32_t audio_sample_rate;
volatile uint32_t audio_channels;
volatile uint32_t audio_frames_played;

static uint16_t LE16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));  //combine 2 byte
}

static uint32_t LE32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); //combine 4 byte
}

static bool ReadExact(void *buffer, UINT size)
{
    UINT received = 0;
    audio_file_result = f_read(&SDFile, buffer, size, &received); //read file sand strore address
    if (audio_file_result != FR_OK || received != size)  //read less byte or cannot read
    {
        audio_state = AUDIO_READ_FAILED;  //debug
        return false;
    }
    return true;
}

static bool Seek(DWORD position) //move in folder
{
    audio_file_result = f_lseek(&SDFile, position);
    if (audio_file_result != FR_OK || f_tell(&SDFile) != position)
    {
        audio_state = AUDIO_READ_FAILED;
        return false;
    }
    return true;
}

static bool ReadHeader(void)  //find , checnk and read wav header
{
    uint8_t header[16];
    uint32_t data_start = 0, data_size = 0;
    uint16_t format = 0, bits = 0, alignment = 0;
    uint32_t byte_rate = 0;
    bool have_format = false, have_data = false;

    if (!ReadExact(header, 12)) return false; //error checking 
    if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))  //check riff , it comapre 2 string and expect 0
        goto invalid;
    uint32_t riff_size = LE32(header + 4);
    if (riff_size < 4 || f_size(&SDFile) < 8 || riff_size > f_size(&SDFile) - 8)
        goto invalid;
    uint32_t end = riff_size + 8;

    while (f_tell(&SDFile) <= end && end - f_tell(&SDFile) >= 8) //continue as long as there are at least 8 bytes leftas teh fiel nto end and read chucnk by chunk
    {
        if (!ReadExact(header, 8)) return false;
        uint32_t size = LE32(header + 4);
        uint32_t start = f_tell(&SDFile);
        if (size > end - start) goto invalid;
        if (!memcmp(header, "fmt ", 4))
        {
            if (size < 16 || have_format) goto invalid;
            if (!ReadExact(header, 16)) return false;
            format = LE16(header);
            audio_channels = LE16(header + 2);
            audio_sample_rate = LE32(header + 4);
            byte_rate = LE32(header + 8);
            alignment = LE16(header + 12);
            bits = LE16(header + 14);
            have_format = true;
        }
        else if (!memcmp(header, "data", 4))
        {
            if (have_data) goto invalid;
            data_start = start;
            data_size = size;
            have_data = true;
        }
        if (have_format && have_data) break;
        /* RIFF chunks are padded to an even byte boundary. */
        if ((size & 1U) && size == end - start) goto invalid;
        if (!Seek(start + size + (size & 1U))) return false;
    }
    if (!have_format || !have_data) goto invalid;
    if (format != 1 || bits != 16 || audio_sample_rate != 48000 ||
        (audio_channels != 1 && audio_channels != 2))
    {
        audio_state = AUDIO_UNSUPPORTED_WAV;
        return false;
    }
    if (alignment != audio_channels * 2 ||
        byte_rate != audio_sample_rate * alignment || data_size % alignment)
        goto invalid;
    remaining = data_size;
    return Seek(data_start);

invalid:
    audio_state = AUDIO_INVALID_WAV;
    return false;
}

static int16_t ApplyVolume(int16_t sample)
{
    int32_t scaled = ((int32_t)sample * (int32_t)playback_volume_percent) / 100;
    if (scaled > INT16_MAX) return INT16_MAX;
    if (scaled < INT16_MIN) return INT16_MIN;
    return (int16_t)scaled;
}

static bool FillHalf(uint32_t half, bool streaming)
{
    uint32_t frame_bytes = audio_channels * 2U;
    uint32_t size = HALF_FRAMES * frame_bytes;
    if (size > remaining) size = remaining;
    if (size && !ReadExact(input, size)) return false;
    /* Do not overwrite a half which DMA has already started reading again. */
    if (streaming && (osThreadFlagsGet() & AUDIO_FLAGS))
    {
        audio_state = AUDIO_UNDERRUN;
        return false;
    }
    int16_t *destination = &output[half * HALF_SAMPLES];
    uint32_t frames = size / frame_bytes;
    for (uint32_t i = 0; i < frames; ++i)
    {
        destination[2U * i] = ApplyVolume(input[audio_channels * i]);
        destination[2U * i + 1U] =
            ApplyVolume(input[audio_channels * i + audio_channels - 1U]);
    }
    memset(destination + frames * 2U, 0,
           (HALF_SAMPLES - frames * 2U) * sizeof(int16_t));
    remaining -= size;
    half_frames[half] = frames;
    last_half[half] = (remaining == 0);
    return true;
}

void AudioPlayer_Play(const char *path, uint8_t volume_percent)
{
    bool file_open = false, dma_running = false;
    playback_volume_percent = volume_percent > 200U ? 200U : volume_percent;
    audio_file_result = FR_OK;
    audio_sai_error = 0;
    audio_frames_played = 0;
    audio_sample_rate = 0;
    audio_channels = 0;
    /* Idle DMA callbacks must not notify us while the next file is prepared. */
    playback_thread = NULL;
    osThreadFlagsClear(AUDIO_FLAGS);

    audio_state = AUDIO_MOUNTING;
    audio_file_result = f_mount(&SDFatFS, SDPath, 1);
    if (audio_file_result != FR_OK)
    {
        audio_state = AUDIO_MOUNT_FAILED;
        goto cleanup;
    }
    audio_state = AUDIO_OPENING;
    audio_file_result = f_open(&SDFile, path, FA_READ);
    if (audio_file_result != FR_OK)
    {
        audio_state = AUDIO_OPEN_FAILED;
        goto cleanup;
    }
    file_open = true;
    audio_state = AUDIO_READING_HEADER;
    if (!ReadHeader()) goto cleanup;
    if (!remaining)
    {
        audio_state = AUDIO_DONE;
        goto cleanup;
    }
    if (idle_silence_running)
    {
        if (HAL_SAI_DMAStop(&hsai_BlockA2) != HAL_OK)
        {
            audio_state = AUDIO_DMA_FAILED;
            goto cleanup;
        }
        idle_silence_running = false;
    }
    if (!FillHalf(0, false) || !FillHalf(1, false)) goto cleanup;
    playback_thread = osThreadGetId();
    osThreadFlagsClear(AUDIO_FLAGS);
    /* DMAStop leaves interrupt flags/masks behind; discard stale events. */
    hsai_BlockA2.Instance->IMR = 0;
    hsai_BlockA2.Instance->CLRFR = 0xFFFFFFFFU;
    HAL_NVIC_SetPriority(SAI2_IRQn, 5, 0);
    HAL_NVIC_ClearPendingIRQ(SAI2_IRQn);
    HAL_NVIC_EnableIRQ(SAI2_IRQn);
    if (HAL_SAI_Transmit_DMA(&hsai_BlockA2, (uint8_t *)output,
                            (uint16_t)(HALF_SAMPLES * 2U)) != HAL_OK)
    {
        audio_state = AUDIO_DMA_FAILED;
        goto cleanup;
    }
    dma_running = true;
    audio_state = AUDIO_PLAYING;
    while (audio_state == AUDIO_PLAYING)
    {
        uint32_t flags = osThreadFlagsWait(AUDIO_FLAGS, osFlagsWaitAny, 1000);
        if (flags & osFlagsError)
        {
            audio_state = AUDIO_TIMEOUT;
            break;
        }
        if (flags & FLAG_ERROR)
        {
            audio_state = AUDIO_DMA_FAILED;
            break;
        }
        if ((flags & (FLAG_HALF | FLAG_FULL)) == (FLAG_HALF | FLAG_FULL))
        {
            audio_state = AUDIO_UNDERRUN;
            break;
        }
        uint32_t half = (flags & FLAG_HALF) ? 0U : 1U;
        audio_frames_played += half_frames[half];
        if (last_half[half])
        {
            /* The other half already contains silence. Clear the consumed
             * final half too, and keep the clocks running with digital zeros.
             * Some amplifiers are noisy when their I2S clocks disappear. */
            memset(&output[half * HALF_SAMPLES], 0,
                   HALF_SAMPLES * sizeof(output[0]));
            idle_silence_running = true;
            dma_running = false; /* Leave this silent transfer running. */
            audio_state = AUDIO_DONE;
            break;
        }
        if (!FillHalf(half, true)) break;
    }

cleanup:
    audio_sai_error = HAL_SAI_GetError(&hsai_BlockA2);
    if (dma_running && HAL_SAI_DMAStop(&hsai_BlockA2) != HAL_OK)
        audio_state = AUDIO_DMA_FAILED;
    HAL_NVIC_DisableIRQ(SAI2_IRQn);
    playback_thread = NULL;
    if (file_open)
    {
        FRESULT close_result = f_close(&SDFile);
        if (close_result != FR_OK && audio_state == AUDIO_DONE)
        {
            audio_file_result = close_result;
            audio_state = AUDIO_READ_FAILED;
        }
    }
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai == &hsai_BlockA2 && playback_thread)
        osThreadFlagsSet(playback_thread, FLAG_HALF);
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai == &hsai_BlockA2 && playback_thread)
        osThreadFlagsSet(playback_thread, FLAG_FULL);
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai == &hsai_BlockA2 && playback_thread)
        osThreadFlagsSet(playback_thread, FLAG_ERROR);
}
