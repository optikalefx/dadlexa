#include "voice_commands.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "audio_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_timer.h"
#include "flite_g2p.h"
#include "model_path.h"
#include "sd_music_library.h"

static const char *TAG = "voice_commands";

#define WAV_HEADER_BYTES 44

static const esp_mn_iface_t *multinet;
static model_iface_data_t *mn_data;
static int chunk_samples;
static int16_t *stream_chunk;
static size_t stream_len;

static int16_t apply_command_gain(int16_t sample)
{
    int32_t amplified = (int32_t)((float)sample * MIC_SOFTWARE_GAIN);
    if (amplified > INT16_MAX) {
        return INT16_MAX;
    }
    if (amplified < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)amplified;
}

static esp_err_t detect_chunk(const int16_t *mono, int *command_id)
{
    esp_mn_state_t state = multinet->detect(mn_data, (int16_t *)mono);
    if (state == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *results = multinet->get_results(mn_data);
        if (results && results->num > 0) {
            *command_id = results->command_id[0];
            ESP_LOGI(TAG, "detected command=%d text=\"%s\" prob=%.3f", *command_id,
                     results->string, results->prob[0]);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t voice_commands_init(void)
{
    ESP_RETURN_ON_FALSE(sd_music_library_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "music library not ready");

    srmodel_list_t *models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(models, ESP_FAIL, TAG, "model load failed");

    char *model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, MULTINET_MODEL_HINT);
    ESP_RETURN_ON_FALSE(model_name, ESP_FAIL, TAG, "no MultiNet model matched %s",
                        MULTINET_MODEL_HINT);

    multinet = esp_mn_handle_from_name(model_name);
    ESP_RETURN_ON_FALSE(multinet, ESP_FAIL, TAG, "MultiNet handle failed");
    mn_data = multinet->create(model_name, VOICE_COMMAND_TIMEOUT_MS);
    ESP_RETURN_ON_FALSE(mn_data, ESP_FAIL, TAG, "MultiNet create failed");
    chunk_samples = multinet->get_samp_chunksize(mn_data);

    ESP_RETURN_ON_ERROR(esp_mn_commands_alloc(multinet, mn_data), TAG, "commands alloc");
    for (size_t i = 0; i < sd_music_library_entry_count(); i++) {
        const sd_song_entry_t *entry = sd_music_library_entry_at(i);
        char *phonemes = flite_g2p(entry->phrase, 1);
        if (!phonemes) {
            ESP_LOGW(TAG, "could not generate phonemes for \"%s\"", entry->phrase);
            continue;
        }

        esp_err_t add_err = esp_mn_commands_phoneme_add(entry->command_id, entry->phrase,
                                                        phonemes);
        if (add_err != ESP_OK) {
            ESP_LOGW(TAG, "could not add command \"%s\"", entry->phrase);
        } else {
            ESP_LOGI(TAG, "command %d: \"%s\" phonemes=\"%s\"", entry->command_id,
                     entry->phrase, phonemes);
        }
        free(phonemes);
    }

    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors) {
        ESP_LOGW(TAG, "MultiNet rejected %d command phrase(s)", errors->num);
    }
    multinet->print_active_speech_commands(mn_data);

    ESP_LOGI(TAG, "ready model=%s chunk=%d rate=%d commands=%d", model_name, chunk_samples,
             multinet->get_samp_rate(mn_data), (int)sd_music_library_entry_count());
    ESP_RETURN_ON_FALSE(multinet->get_samp_rate(mn_data) == RECORD_SAMPLE_RATE, ESP_FAIL, TAG,
                        "unexpected MultiNet sample rate");
    return ESP_OK;
}

bool voice_commands_ready(void)
{
    return multinet && mn_data && chunk_samples > 0;
}

esp_err_t voice_commands_listen(int *command_id)
{
    ESP_RETURN_ON_FALSE(command_id, ESP_ERR_INVALID_ARG, TAG, "command id missing");
    ESP_RETURN_ON_FALSE(voice_commands_ready(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    *command_id = 0;

    int16_t *mono = malloc(chunk_samples * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(mono, ESP_ERR_NO_MEM, TAG, "command buffer");

    multinet->clean(mn_data);
    int64_t deadline = esp_timer_get_time() + (int64_t)VOICE_COMMAND_TIMEOUT_MS * 1000;
    esp_err_t err = ESP_ERR_TIMEOUT;
    while (esp_timer_get_time() < deadline) {
        err = audio_board_read_mono_gain(mono, chunk_samples, MIC_SOFTWARE_GAIN);
        if (err != ESP_OK) {
            break;
        }

        esp_mn_state_t state = multinet->detect(mn_data, mono);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *results = multinet->get_results(mn_data);
            if (results && results->num > 0) {
                *command_id = results->command_id[0];
                ESP_LOGI(TAG, "detected command=%d text=\"%s\" prob=%.3f", *command_id,
                         results->string, results->prob[0]);
                err = ESP_OK;
                break;
            }
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
    }

    free(mono);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGI(TAG, "command listen timed out");
    }
    return err;
}

esp_err_t voice_commands_detect_recording(const recorded_audio_t *audio, int *command_id)
{
    ESP_RETURN_ON_FALSE(command_id, ESP_ERR_INVALID_ARG, TAG, "command id missing");
    ESP_RETURN_ON_FALSE(voice_commands_ready(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(audio && audio->ok && audio->wav && audio->wav_size > WAV_HEADER_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid audio");
    *command_id = 0;

    int16_t *chunk = malloc(chunk_samples * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(chunk, ESP_ERR_NO_MEM, TAG, "command replay buffer");

    multinet->clean(mn_data);
    const int16_t *samples = (const int16_t *)(audio->wav + WAV_HEADER_BYTES);
    size_t sample_count = (audio->wav_size - WAV_HEADER_BYTES) / sizeof(int16_t);
    esp_err_t err = ESP_ERR_NOT_FOUND;

    for (size_t offset = 0; offset < sample_count; offset += (size_t)chunk_samples) {
        size_t copy = sample_count - offset;
        if (copy > (size_t)chunk_samples) {
            copy = (size_t)chunk_samples;
        }
        for (size_t i = 0; i < copy; i++) {
            chunk[i] = apply_command_gain(samples[offset + i]);
        }
        if (copy < (size_t)chunk_samples) {
            memset(chunk + copy, 0, (chunk_samples - copy) * sizeof(int16_t));
        }

        err = detect_chunk(chunk, command_id);
        if (err == ESP_OK) {
            break;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no song command detected in recording");
    }
    free(chunk);
    return err;
}

esp_err_t voice_commands_stream_begin(void)
{
    ESP_RETURN_ON_FALSE(voice_commands_ready(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (!stream_chunk) {
        stream_chunk = malloc(chunk_samples * sizeof(int16_t));
        ESP_RETURN_ON_FALSE(stream_chunk, ESP_ERR_NO_MEM, TAG, "stream buffer");
    }
    stream_len = 0;
    multinet->clean(mn_data);
    return ESP_OK;
}

esp_err_t voice_commands_stream_feed(const int16_t *samples, size_t sample_count, int *command_id)
{
    ESP_RETURN_ON_FALSE(samples || sample_count == 0, ESP_ERR_INVALID_ARG, TAG, "samples missing");
    ESP_RETURN_ON_FALSE(command_id, ESP_ERR_INVALID_ARG, TAG, "command id missing");
    ESP_RETURN_ON_FALSE(stream_chunk, ESP_ERR_INVALID_STATE, TAG, "stream not started");
    if (*command_id > 0) {
        return ESP_OK;
    }

    size_t offset = 0;
    while (offset < sample_count) {
        size_t copy = (size_t)chunk_samples - stream_len;
        if (copy > sample_count - offset) {
            copy = sample_count - offset;
        }
        for (size_t i = 0; i < copy; i++) {
            stream_chunk[stream_len + i] = apply_command_gain(samples[offset + i]);
        }
        stream_len += copy;
        offset += copy;

        if (stream_len == (size_t)chunk_samples) {
            esp_err_t err = detect_chunk(stream_chunk, command_id);
            stream_len = 0;
            if (err == ESP_OK) {
                return ESP_OK;
            }
        }
    }
    return ESP_OK;
}

esp_err_t voice_commands_stream_end(int *command_id)
{
    ESP_RETURN_ON_FALSE(command_id, ESP_ERR_INVALID_ARG, TAG, "command id missing");
    ESP_RETURN_ON_FALSE(stream_chunk, ESP_ERR_INVALID_STATE, TAG, "stream not started");
    if (*command_id == 0 && stream_len > 0) {
        memset(stream_chunk + stream_len, 0, ((size_t)chunk_samples - stream_len) * sizeof(int16_t));
        (void)detect_chunk(stream_chunk, command_id);
    }
    stream_len = 0;
    if (*command_id == 0) {
        ESP_LOGI(TAG, "no song command detected in recording");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
