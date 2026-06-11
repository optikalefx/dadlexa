#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t play_micro_opus_ogg(const uint8_t *ogg_data, size_t ogg_size);
esp_err_t play_micro_opus_ogg_with_gain(const uint8_t *ogg_data, size_t ogg_size, float gain);

#ifdef __cplusplus
}
#endif
