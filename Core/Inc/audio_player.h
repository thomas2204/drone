#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "fatfs.h"

typedef enum {
    AUDIO_IDLE,
    AUDIO_MOUNTING,
    AUDIO_OPENING,
    AUDIO_READING_HEADER,
    AUDIO_PLAYING,
    AUDIO_DONE,
    AUDIO_MOUNT_FAILED,
    AUDIO_OPEN_FAILED,
    AUDIO_READ_FAILED,
    AUDIO_INVALID_WAV,
    AUDIO_UNSUPPORTED_WAV,
    AUDIO_DMA_FAILED,
    AUDIO_UNDERRUN,
    AUDIO_TIMEOUT
} AudioPlayerState;

/* Debugger-visible results; only one task may use the player at a time. */
extern volatile AudioPlayerState audio_state;
extern volatile FRESULT audio_file_result;
extern volatile uint32_t audio_sai_error;
extern volatile uint32_t audio_sample_rate;
extern volatile uint32_t audio_channels;
extern volatile uint32_t audio_frames_played;

/* Plays once, blocking the calling RTOS task while DMA transmits audio.
 * Supports 48 kHz, signed 16-bit PCM WAV, mono or stereo.
 * Volume is linear sample gain: 0 = mute, 100 = original level.
 * Values above 100 are clamped to 100.
 * D-cache must remain disabled, as in the current project startup. */
void AudioPlayer_Play(const char *path, uint8_t volume_percent);

#endif
