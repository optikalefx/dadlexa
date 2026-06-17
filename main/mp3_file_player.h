#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mp3_file_player_play(const char *path);
bool mp3_file_player_is_playing(void);
void mp3_file_player_request_stop(void);

#ifdef __cplusplus
}
#endif
