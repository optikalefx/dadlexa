#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    uint8_t *wav;
    size_t wav_size;
    uint32_t duration_ms;
    uint32_t rms;
    int16_t peak;
} recorded_audio_t;

esp_err_t audio_board_init(void);
void audio_board_lock(void);
void audio_board_unlock(void);
esp_err_t audio_board_prepare_recording(void);
esp_err_t audio_board_prepare_playback_48k(void);
esp_err_t audio_board_read_stereo(int16_t *buffer, size_t frames, uint32_t timeout_ms);
esp_err_t audio_board_read_mono_gain(int16_t *buffer, size_t samples, float gain);
esp_err_t audio_board_record_until_silence(recorded_audio_t *audio);
esp_err_t audio_board_play_tone(uint32_t frequency_hz, uint32_t duration_ms);
esp_err_t audio_board_write_pcm(const int16_t *pcm, size_t samples);
esp_err_t audio_board_read_k1(bool *pressed);
esp_err_t audio_board_read_k2(bool *pressed);
void audio_board_release_recording(recorded_audio_t *audio);
esp_codec_dev_handle_t audio_board_speaker_codec(void);

#ifdef __cplusplus
}
#endif
