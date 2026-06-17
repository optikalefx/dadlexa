#include "mp3_file_player.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_config.h"
#include "audio_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"

#define MP3_INPUT_BUFFER_BYTES (15 * 1024)
#define MP3_OUTPUT_BUFFER_BYTES (8 * 1024)
#define MP3_WRITE_CHUNK_SAMPLES 1024

static const char *TAG = "mp3_player";

static volatile bool playback_active;
static volatile bool stop_requested;

static esp_err_t audio_err_to_esp(esp_audio_err_t err)
{
    return err == ESP_AUDIO_ERR_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t locked_prepare_playback(int sample_rate, int channels)
{
    audio_board_lock();
    esp_err_t err = audio_board_prepare_playback(sample_rate, channels);
    audio_board_unlock();
    return err;
}

static esp_err_t locked_write_pcm(const int16_t *samples, size_t sample_count)
{
    audio_board_lock();
    esp_err_t err = audio_board_write_pcm(samples, sample_count);
    audio_board_unlock();
    return err;
}

static void locked_prepare_recording(void)
{
    audio_board_lock();
    audio_board_prepare_recording();
    audio_board_unlock();
}

static bool locked_k1_pressed(void)
{
    bool pressed = false;
    audio_board_lock();
    esp_err_t err = audio_board_read_k1(&pressed);
    audio_board_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "K1 read failed during MP3 playback: %s", esp_err_to_name(err));
        return false;
    }
    return pressed;
}

bool mp3_file_player_is_playing(void)
{
    return playback_active;
}

void mp3_file_player_request_stop(void)
{
    if (playback_active) {
        stop_requested = true;
    }
}

esp_err_t mp3_file_player_play(const char *path)
{
    ESP_RETURN_ON_FALSE(path && path[0], ESP_ERR_INVALID_ARG, TAG, "missing path");

    FILE *file = fopen(path, "rb");
    if (!file) {
        int open_errno = errno;
        struct stat st = {0};
        int stat_result = stat(path, &st);
        ESP_LOGE(TAG, "open %s failed errno=%d stat=%d size=%ld", path, open_errno,
                 stat_result, stat_result == 0 ? (long)st.st_size : -1L);
        return ESP_ERR_NOT_FOUND;
    }

    void *decoder = NULL;
    uint8_t *input = malloc(MP3_INPUT_BUFFER_BYTES);
    uint8_t *output = malloc(MP3_OUTPUT_BUFFER_BYTES);
    size_t input_len = 0;
    size_t output_len = MP3_OUTPUT_BUFFER_BYTES;
    bool speaker_ready = false;
    bool decoded_any = false;
    bool playback_started = false;
    bool stopped = false;
    esp_err_t result = ESP_OK;

    if (!input || !output) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    result = audio_err_to_esp(esp_mp3_dec_open(NULL, 0, &decoder));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "decoder open failed");
        goto cleanup;
    }

    result = locked_prepare_playback(PLAYBACK_SAMPLE_RATE, 2);
    if (result != ESP_OK) {
        goto cleanup;
    }
    playback_started = true;
    stop_requested = false;
    playback_active = true;

    ESP_LOGI(TAG, "playing %s", path);
    while (!stop_requested) {
        if (locked_k1_pressed()) {
            stop_requested = true;
            break;
        }

        if (input_len < MP3_INPUT_BUFFER_BYTES) {
            size_t read = fread(input + input_len, 1, MP3_INPUT_BUFFER_BYTES - input_len, file);
            input_len += read;
        }
        if (input_len == 0) {
            break;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer = input,
            .len = input_len,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = output,
            .len = output_len,
        };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t dec_err = esp_mp3_dec_decode(decoder, &raw, &frame, &info);

        if (dec_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint8_t *larger = realloc(output, frame.needed_size);
            if (!larger) {
                result = ESP_ERR_NO_MEM;
                break;
            }
            output = larger;
            output_len = frame.needed_size;
            continue;
        }
        if (dec_err == ESP_AUDIO_ERR_DATA_LACK) {
            if (feof(file)) {
                break;
            }
            if (raw.consumed > 0 && raw.consumed <= input_len) {
                memmove(input, input + raw.consumed, input_len - raw.consumed);
                input_len -= raw.consumed;
            }
            continue;
        }
        if (dec_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "decode failed: %d", dec_err);
            result = ESP_FAIL;
            break;
        }

        if (!speaker_ready && info.sample_rate > 0 && (info.channel == 1 || info.channel == 2)) {
            result = locked_prepare_playback(info.sample_rate, info.channel);
            if (result != ESP_OK) {
                break;
            }
            speaker_ready = true;
            ESP_LOGI(TAG, "mp3 info rate=%" PRIu32 " bits=%u channels=%u", info.sample_rate,
                     info.bits_per_sample, info.channel);
        }

        if (frame.decoded_size > 0) {
            const int16_t *samples = (const int16_t *)frame.buffer;
            size_t sample_count = frame.decoded_size / sizeof(int16_t);
            size_t written = 0;
            while (written < sample_count && !stop_requested) {
                if (locked_k1_pressed()) {
                    stop_requested = true;
                    break;
                }
                size_t chunk = sample_count - written;
                if (chunk > MP3_WRITE_CHUNK_SAMPLES) {
                    chunk = MP3_WRITE_CHUNK_SAMPLES;
                }
                result = locked_write_pcm(samples + written, chunk);
                if (result != ESP_OK) {
                    break;
                }
                written += chunk;
            }
            decoded_any = true;
            if (result != ESP_OK || stop_requested) {
                break;
            }
        }

        if (raw.consumed > input_len) {
            result = ESP_FAIL;
            break;
        }
        memmove(input, input + raw.consumed, input_len - raw.consumed);
        input_len -= raw.consumed;
        if (raw.consumed == 0 && frame.decoded_size == 0 && feof(file)) {
            break;
        }
    }

    if (stop_requested) {
        stopped = true;
        ESP_LOGI(TAG, "stopped %s", path);
    }

    if (playback_started) {
        locked_prepare_recording();
    }
    playback_active = false;
    stop_requested = false;

    if (result == ESP_OK && !decoded_any && !stopped) {
        result = ESP_FAIL;
    }

cleanup:
    playback_active = false;
    stop_requested = false;
    if (decoder) {
        esp_mp3_dec_close(decoder);
    }
    free(output);
    free(input);
    fclose(file);
    return result;
}
