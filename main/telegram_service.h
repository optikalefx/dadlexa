#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t telegram_send_message(const char *message);
esp_err_t telegram_send_voice_ogg(const uint8_t *ogg, size_t ogg_size, const char *caption);
esp_err_t telegram_get_next_update_offset(int64_t *offset);
esp_err_t telegram_poll_reply_file(int64_t *offset, char *file_id, size_t file_id_size);
esp_err_t telegram_download_file_by_id(const char *file_id, uint8_t **data, size_t *size);
