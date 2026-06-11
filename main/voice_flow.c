#include "voice_flow.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "audio_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_ring.h"
#include "micro_opus_player.h"
#include "telegram_service.h"

static const char *TAG = "voice_flow";

#define WAKE_TONE_HZ 880
#define WAKE_TONE_MS 120
#define DONE_TONE_HZ 440
#define DONE_TONE_MS 180
#define CHASE_STEP_MS 140

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

static void chase_task(void *arg)
{
    chase_status_t *status = (chase_status_t *)arg;
    while (status->running) {
        led_ring_chase_step(status->red, status->green, status->blue);
        vTaskDelay(pdMS_TO_TICKS(status->step_ms));
    }
    vTaskDelete(NULL);
}

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

static void stop_chase(chase_status_t *status)
{
    if (!status->running) {
        return;
    }
    status->running = false;
    vTaskDelay(pdMS_TO_TICKS(status->step_ms + 20));
}

static void upload_and_poll_task(void *arg)
{
    upload_job_t *job = (upload_job_t *)arg;
    int64_t offset = 0;
    chase_status_t chase = {0};

    if (telegram_get_next_update_offset(&offset) != ESP_OK) {
        ESP_LOGW(TAG, "could not prime Telegram offset; using 0");
    }

    start_chase(&chase, "upload_led", 0, 255, 0);
    esp_err_t sent = telegram_send_audio(&job->audio, "Recorded audio");
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
                audio_board_lock();
                if (audio_board_prepare_playback_48k() == ESP_OK) {
                    stop_chase(&chase);
                    led_ring_flash(0, 255, 0, 80, 40, 1);
                    esp_err_t played = play_micro_opus_ogg(data, size);
                    ESP_LOGI(TAG, "reply playback %s", played == ESP_OK ? "ok" : "failed");
                }
                audio_board_prepare_recording();
                audio_board_unlock();
                free(data);
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
    led_ring_clear();

    if (err != ESP_OK || !audio.ok) {
        ESP_LOGW(TAG, "recording failed: %s", esp_err_to_name(err));
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
    if (xTaskCreatePinnedToCore(upload_and_poll_task, "upload_poll", 12288, job, 4, &task, 0) !=
        pdPASS) {
        audio_board_release_recording(&job->audio);
        free(job);
    }
}
