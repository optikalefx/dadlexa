#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_board.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t duration_ms;
} ogg_opus_audio_t;

esp_err_t ogg_opus_encode_recording(const recorded_audio_t *recording, ogg_opus_audio_t *out);
void ogg_opus_release(ogg_opus_audio_t *audio);

#ifdef __cplusplus
}
#endif
