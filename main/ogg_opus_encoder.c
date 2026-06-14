#include "ogg_opus_encoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_muxer.h"
#include "esp_muxer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ogg_muxer.h"
#include "opus.h"

#define WAV_HEADER_BYTES 44
#define OPUS_FRAME_MS 20
#define OPUS_FRAME_SAMPLES (RECORD_SAMPLE_RATE * OPUS_FRAME_MS / 1000)
#define OPUS_MAX_PACKET_BYTES 400

static const char *TAG = "ogg_opus";

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} mux_buffer_t;

/* Data preparation helper: grows the heap-backed Ogg output buffer and appends
 * bytes emitted by the muxer. */
static esp_err_t append_bytes(mux_buffer_t *buf, const void *data, size_t size)
{
    if (buf->size + size > buf->cap) {
        size_t next = buf->cap == 0 ? 4096 : buf->cap * 2;
        while (next < buf->size + size) {
            next *= 2;
        }
        uint8_t *resized = heap_caps_realloc(buf->data, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!resized) {
            resized = realloc(buf->data, next);
        }
        ESP_RETURN_ON_FALSE(resized, ESP_ERR_NO_MEM, TAG, "opus output realloc");
        buf->data = resized;
        buf->cap = next;
    }
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
    return ESP_OK;
}

/* Data preparation callback: bridges esp_muxer output into the in-memory buffer
 * that will later be uploaded to Telegram. */
static int muxer_data_cb(esp_muxer_data_info_t *data, void *ctx)
{
    mux_buffer_t *buf = (mux_buffer_t *)ctx;
    if (!data || !data->data || data->size == 0 || !buf) {
        return -1;
    }
    return append_bytes(buf, data->data, data->size) == ESP_OK ? 0 : -1;
}

/* Data preparation workflow: converts the recorded WAV/PCM buffer into an Ogg
 * Opus voice payload suitable for Telegram upload. */
esp_err_t ogg_opus_encode_recording(const recorded_audio_t *recording, ogg_opus_audio_t *out)
{
    memset(out, 0, sizeof(*out));
    ESP_RETURN_ON_FALSE(recording && recording->ok && recording->wav &&
                            recording->wav_size > WAV_HEADER_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid recording");

    const int16_t *pcm = (const int16_t *)(recording->wav + WAV_HEADER_BYTES);
    size_t sample_count = (recording->wav_size - WAV_HEADER_BYTES) / sizeof(int16_t);
    ESP_RETURN_ON_FALSE(sample_count > 0, ESP_ERR_INVALID_ARG, TAG, "empty recording");

    int opus_error = OPUS_OK;
    OpusEncoder *encoder = opus_encoder_create(RECORD_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP,
                                               &opus_error);
    ESP_RETURN_ON_FALSE(encoder && opus_error == OPUS_OK, ESP_FAIL, TAG, "opus encoder");
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1));

    mux_buffer_t muxed = {0};
    ogg_muxer_config_t mux_cfg = {0};
    mux_cfg.base_config.muxer_type = ESP_MUXER_TYPE_OGG;
    mux_cfg.base_config.slice_duration = recording->duration_ms + 1000;
    mux_cfg.base_config.data_cb = muxer_data_cb;
    mux_cfg.base_config.ctx = &muxed;
    mux_cfg.base_config.ram_cache_size = 4096;
    mux_cfg.page_cache_size = 4096;

    esp_muxer_register_default();
    esp_muxer_handle_t muxer = esp_muxer_open(&mux_cfg.base_config, sizeof(mux_cfg));
    if (!muxer) {
        opus_encoder_destroy(encoder);
        esp_muxer_unreg_all();
        return ESP_FAIL;
    }

    esp_muxer_audio_stream_info_t stream_info = {
        .codec = ESP_MUXER_ADEC_OPUS,
        .channel = 1,
        .bits_per_sample = 16,
        .sample_rate = RECORD_SAMPLE_RATE,
        .min_packet_duration = OPUS_FRAME_MS,
    };
    int stream_idx = 0;
    esp_err_t err = esp_muxer_add_audio_stream(muxer, &stream_info, &stream_idx) == 0
                        ? ESP_OK
                        : ESP_FAIL;

    uint8_t packet[OPUS_MAX_PACKET_BYTES];
    int16_t frame[OPUS_FRAME_SAMPLES];
    size_t offset = 0;
    while (err == ESP_OK && offset < sample_count) {
        size_t remaining = sample_count - offset;
        size_t frame_samples = remaining > OPUS_FRAME_SAMPLES ? OPUS_FRAME_SAMPLES : remaining;
        memcpy(frame, pcm + offset, frame_samples * sizeof(int16_t));
        if (frame_samples < OPUS_FRAME_SAMPLES) {
            memset(frame + frame_samples, 0,
                   (OPUS_FRAME_SAMPLES - frame_samples) * sizeof(int16_t));
        }

        opus_int32 packet_len = opus_encode(encoder, frame, OPUS_FRAME_SAMPLES, packet,
                                            sizeof(packet));
        if (packet_len < 0) {
            err = ESP_FAIL;
            break;
        }

        offset += frame_samples;
        esp_muxer_audio_packet_t audio_packet = {
            .data = packet,
            .len = packet_len,
            .pts = (uint32_t)((uint64_t)offset * 1000 / RECORD_SAMPLE_RATE),
        };
        err = esp_muxer_add_audio_packet(muxer, stream_idx, &audio_packet) == 0 ? ESP_OK : ESP_FAIL;
        vTaskDelay(1);
    }

    opus_encoder_destroy(encoder);
    if (esp_muxer_close(muxer) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    esp_muxer_unreg_all();
    if (err != ESP_OK) {
        free(muxed.data);
        return err;
    }

    out->data = muxed.data;
    out->size = muxed.size;
    out->duration_ms = recording->duration_ms;
    ESP_LOGI(TAG, "encoded wav=%u opus=%u duration=%lu ms",
             (unsigned)recording->wav_size, (unsigned)out->size, (unsigned long)out->duration_ms);
    return ESP_OK;
}

/* Data ownership helper: frees an encoded Ogg Opus buffer once it is no longer
 * needed or has not been retained for replay. */
void ogg_opus_release(ogg_opus_audio_t *audio)
{
    if (audio && audio->data) {
        free(audio->data);
        memset(audio, 0, sizeof(*audio));
    }
}
