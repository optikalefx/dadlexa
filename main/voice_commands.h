#pragma once

#include <stdbool.h>

#include "audio_board.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_commands_init(void);
bool voice_commands_ready(void);
esp_err_t voice_commands_listen(int *command_id);
esp_err_t voice_commands_detect_recording(const recorded_audio_t *audio, int *command_id);
esp_err_t voice_commands_stream_begin(void);
esp_err_t voice_commands_stream_feed(const int16_t *samples, size_t sample_count, int *command_id);
esp_err_t voice_commands_stream_end(int *command_id);

#ifdef __cplusplus
}
#endif
