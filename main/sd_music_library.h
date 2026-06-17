#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int command_id;
    char phrase[64];
    char song_path[160];
} sd_song_entry_t;

esp_err_t sd_music_library_init(void);
esp_err_t sd_music_library_mount(void);
bool sd_music_library_ready(void);
size_t sd_music_library_entry_count(void);
const sd_song_entry_t *sd_music_library_entry_at(size_t index);
const sd_song_entry_t *sd_music_library_find_command(int command_id);

#ifdef __cplusplus
}
#endif
