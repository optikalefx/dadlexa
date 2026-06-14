#include "voice_flow.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "audio_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_ring.h"
#include "micro_opus_player.h"
#include "ogg_opus_encoder.h"
#include "telegram_service.h"

static const char *TAG = "voice_flow";

#define WAKE_TONE_HZ 880
#define WAKE_TONE_MS 120
#define DONE_TONE_HZ 440
#define DONE_TONE_MS 180
#define CHASE_STEP_MS 140
#define BUTTON_POLL_MS 50
#define BUTTON_DEBOUNCE_MS 250
#define SENT_PLAYBACK_GAIN 4.0f

typedef struct {
    recorded_audio_t audio;
} upload_job_t;

typedef struct {
    volatile bool running;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint32_t step_ms;
} chase_status_t;

static SemaphoreHandle_t audio_cache_mutex;
static uint8_t *last_reply_ogg;
static size_t last_reply_ogg_size;
static uint8_t *last_sent_ogg;
static size_t last_sent_ogg_size;
static bool button_task_started;

/* Coordination helper: lazily creates the mutex protecting cached Telegram reply
 * and sent-audio buffers. */
static esp_err_t ensure_audio_cache_mutex(void)
{
    if (!audio_cache_mutex) {
        audio_cache_mutex = xSemaphoreCreateMutex();
    }
    return audio_cache_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

/* LED hardware task: runs a repeating chase animation while upload or Telegram
 * polling work is in progress. */
static void chase_task(void *arg)
{
    chase_status_t *status = (chase_status_t *)arg;
    while (status->running) {
        led_ring_chase_step(status->red, status->green, status->blue);
        vTaskDelay(pdMS_TO_TICKS(status->step_ms));
    }
    vTaskDelete(NULL);
}

/* LED hardware helper: configures chase color/timing and starts the FreeRTOS
 * task that drives the physical LED ring. */
static void start_chase(chase_status_t *status, const char *name, uint8_t red, uint8_t green,
                        uint8_t blue)
{
    status->red = red;
    status->green = green;
    status->blue = blue;
    status->step_ms = CHASE_STEP_MS;
    status->running = true;
    if (xTaskCreatePinnedToCore(chase_task, name, 2048, status, 2, NULL, 0) != pdPASS) {
        status->running = false;
    }
}

/* LED hardware helper: stops a chase task and waits long enough for it to exit
 * before another status pattern takes over the ring. */
static void stop_chase(chase_status_t *status)
{
    if (!status->running) {
        return;
    }
    status->running = false;
    vTaskDelay(pdMS_TO_TICKS(status->step_ms + 20));
}

/* Data ownership helper: replaces the cached Telegram reply Ogg buffer so K1 can
 * replay the latest downloaded response. */
static esp_err_t retain_last_reply(uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(data && size, ESP_ERR_INVALID_ARG, TAG, "invalid reply");
    ESP_RETURN_ON_ERROR(ensure_audio_cache_mutex(), TAG, "cache mutex");

    xSemaphoreTake(audio_cache_mutex, portMAX_DELAY);
    free(last_reply_ogg);
    last_reply_ogg = data;
    last_reply_ogg_size = size;
    xSemaphoreGive(audio_cache_mutex);
    return ESP_OK;
}

/* Data ownership helper: replaces the cached outgoing Ogg buffer so K2 can
 * replay what was last uploaded to Telegram. */
static esp_err_t retain_last_sent(uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(data && size, ESP_ERR_INVALID_ARG, TAG, "invalid sent audio");
    ESP_RETURN_ON_ERROR(ensure_audio_cache_mutex(), TAG, "cache mutex");

    xSemaphoreTake(audio_cache_mutex, portMAX_DELAY);
    free(last_sent_ogg);
    last_sent_ogg = data;
    last_sent_ogg_size = size;
    xSemaphoreGive(audio_cache_mutex);
    return ESP_OK;
}

/* Hardware playback helper: replays the cached Telegram reply from RAM through
 * the speaker path and flashes status LEDs. */
static esp_err_t play_last_reply_from_ram(void)
{
    ESP_RETURN_ON_ERROR(ensure_audio_cache_mutex(), TAG, "cache mutex");

    xSemaphoreTake(audio_cache_mutex, portMAX_DELAY);
    if (!last_reply_ogg || last_reply_ogg_size == 0) {
        xSemaphoreGive(audio_cache_mutex);
        ESP_LOGI(TAG, "no Telegram reply cached yet");
        led_ring_flash(255, 0, 0, 80, 40, 1);
        return ESP_ERR_NOT_FOUND;
    }

    audio_board_lock();
    esp_err_t err = audio_board_prepare_playback_48k();
    if (err == ESP_OK) {
        led_ring_flash(0, 255, 0, 80, 40, 1);
        err = play_micro_opus_ogg(last_reply_ogg, last_reply_ogg_size);
        ESP_LOGI(TAG, "cached reply playback %s", err == ESP_OK ? "ok" : "failed");
    }
    audio_board_prepare_recording();
    audio_board_unlock();

    xSemaphoreGive(audio_cache_mutex);
    return err;
}

/* Hardware playback helper: replays the most recently uploaded voice recording
 * from RAM through the speaker path with extra gain. */
static esp_err_t play_last_sent_from_ram(void)
{
    ESP_RETURN_ON_ERROR(ensure_audio_cache_mutex(), TAG, "cache mutex");

    xSemaphoreTake(audio_cache_mutex, portMAX_DELAY);
    if (!last_sent_ogg || last_sent_ogg_size == 0) {
        xSemaphoreGive(audio_cache_mutex);
        ESP_LOGI(TAG, "no sent recording cached yet");
        led_ring_flash(255, 0, 0, 80, 40, 1);
        return ESP_ERR_NOT_FOUND;
    }

    audio_board_lock();
    esp_err_t err = audio_board_prepare_playback_48k();
    if (err == ESP_OK) {
        led_ring_flash(0, 180, 255, 80, 40, 1);
        err = play_micro_opus_ogg_with_gain(last_sent_ogg, last_sent_ogg_size,
                                            SENT_PLAYBACK_GAIN);
        ESP_LOGI(TAG, "cached sent playback %s", err == ESP_OK ? "ok" : "failed");
    }
    audio_board_prepare_recording();
    audio_board_unlock();

    xSemaphoreGive(audio_cache_mutex);
    return err;
}

/* Hardware input task: polls the physical K1/K2 buttons via audio_board and
 * routes them to cached reply or sent-audio playback. */
static void button_task(void *arg)
{
    (void)arg;
    bool k1_was_pressed = false;
    bool k2_was_pressed = false;
    int64_t k1_last_press_us = 0;
    int64_t k2_last_press_us = 0;

    while (true) {
        bool k1_pressed = false;
        bool k2_pressed = false;
        audio_board_lock();
        esp_err_t k1_err = audio_board_read_k1(&k1_pressed);
        esp_err_t k2_err = audio_board_read_k2(&k2_pressed);
        audio_board_unlock();

        int64_t now = esp_timer_get_time();
        if (k1_err == ESP_OK && k1_pressed && !k1_was_pressed &&
            now - k1_last_press_us >= (int64_t)BUTTON_DEBOUNCE_MS * 1000) {
            k1_last_press_us = now;
            play_last_reply_from_ram();
        } else if (k1_err != ESP_OK) {
            ESP_LOGW(TAG, "K1 read failed: %s", esp_err_to_name(k1_err));
        }
        if (k2_err == ESP_OK && k2_pressed && !k2_was_pressed &&
            now - k2_last_press_us >= (int64_t)BUTTON_DEBOUNCE_MS * 1000) {
            k2_last_press_us = now;
            play_last_sent_from_ram();
        } else if (k2_err != ESP_OK) {
            ESP_LOGW(TAG, "K2 read failed: %s", esp_err_to_name(k2_err));
        }
        k1_was_pressed = k1_pressed;
        k2_was_pressed = k2_pressed;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

/* Hardware input boundary: starts the background button polling task once so the
 * user can replay cached audio without waking the voice flow. */
esp_err_t voice_flow_start_button_task(void)
{
    ESP_RETURN_ON_ERROR(ensure_audio_cache_mutex(), TAG, "cache mutex");
    if (button_task_started) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(button_task, "button_k1", 4096, NULL, 3, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "button task");
    button_task_started = true;
    return ESP_OK;
}

/* Business-critical Telegram workflow: encodes the recorded WAV to Ogg Opus,
 * uploads it to Telegram, polls for a reply file, downloads that reply, caches
 * it, and plays it on the speaker. */
static void upload_and_poll_task(void *arg)
{
    upload_job_t *job = (upload_job_t *)arg;
    int64_t offset = 0;
    chase_status_t chase = {0};

    start_chase(&chase, "upload_led", 0, 255, 0);
    if (telegram_get_next_update_offset(&offset) != ESP_OK) {
        ESP_LOGW(TAG, "could not prime Telegram offset; using 0");
    }

    ogg_opus_audio_t opus = {0};
    esp_err_t sent = ESP_FAIL;
    if (ogg_opus_encode_recording(&job->audio, &opus) == ESP_OK) {
        sent = telegram_send_voice_ogg(opus.data, opus.size, "Recorded audio");
        if (sent == ESP_OK && retain_last_sent(opus.data, opus.size) == ESP_OK) {
            memset(&opus, 0, sizeof(opus));
        }
        ogg_opus_release(&opus);
    }
    if (sent != ESP_OK) {
        ESP_LOGW(TAG, "Opus upload failed");
    }
    stop_chase(&chase);
    audio_board_release_recording(&job->audio);
    if (sent != ESP_OK) {
        ESP_LOGW(TAG, "audio upload failed");
        led_ring_clear();
        free(job);
        vTaskDelete(NULL);
    }
    led_ring_flash(0, 0, 255, 100, 70, 2);

    int64_t deadline = esp_timer_get_time() + (int64_t)TELEGRAM_REPLY_WAIT_TIMEOUT_MS * 1000;
    start_chase(&chase, "poll_led", 255, 180, 0);
    while (esp_timer_get_time() < deadline) {
        char file_id[256] = {0};
        esp_err_t poll = telegram_poll_reply_file(&offset, file_id, sizeof(file_id));
        if (poll == ESP_OK) {
            uint8_t *data = NULL;
            size_t size = 0;
            if (telegram_download_file_by_id(file_id, &data, &size) == ESP_OK) {
                stop_chase(&chase);
                if (retain_last_reply(data, size) == ESP_OK) {
                    play_last_reply_from_ram();
                } else {
                    free(data);
                }
            }
            break;
        }
        stop_chase(&chase);
        led_ring_flash(0, 0, 255, 80, 40, 1);
        start_chase(&chase, "poll_led", 255, 180, 0);
        vTaskDelay(pdMS_TO_TICKS(TELEGRAM_REPLY_POLL_INTERVAL_MS));
    }

    stop_chase(&chase);
    led_ring_clear();
    free(job);
    vTaskDelete(NULL);
}

/* Business-critical voice workflow: after wake-word detection, gives audible/LED
 * cues, records speech from the microphone, and starts the Telegram upload/reply
 * task if recording succeeded. */
void voice_flow_handle_wake(void)
{
    recorded_audio_t audio = {0};

    audio_board_lock();
    led_ring_set(0, 255, 0);
    audio_board_play_tone(WAKE_TONE_HZ, WAKE_TONE_MS);
    esp_err_t err = audio_board_record_until_silence(&audio);
    led_ring_set(0, 0, 255);
    audio_board_play_tone(DONE_TONE_HZ, DONE_TONE_MS);
    audio_board_unlock();

    vTaskDelay(pdMS_TO_TICKS(500));

    if (err != ESP_OK || !audio.ok) {
        ESP_LOGW(TAG, "recording failed: %s", esp_err_to_name(err));
        led_ring_clear();
        telegram_send_message("Voice recording failed.");
        return;
    }

    upload_job_t *job = calloc(1, sizeof(upload_job_t));
    if (!job) {
        audio_board_release_recording(&audio);
        return;
    }
    job->audio = audio;

    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCore(upload_and_poll_task, "upload_poll", 24576, job, 4, &task, 0) !=
        pdPASS) {
        audio_board_release_recording(&job->audio);
        free(job);
    }
}
