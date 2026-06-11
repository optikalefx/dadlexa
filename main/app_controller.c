#include "app_controller.h"

#include "audio_board.h"
#include "esp_log.h"
#include "led_ring.h"
#include "telegram_service.h"
#include "voice_flow.h"
#include "wake_word.h"
#include "wifi_service.h"

static const char *TAG = "app";

void app_controller_run(void)
{
    ESP_ERROR_CHECK(led_ring_init());
    ESP_ERROR_CHECK(led_ring_clear());
    ESP_ERROR_CHECK(wifi_service_start());
    ESP_LOGI(TAG, "wifi connected");

    if (telegram_send_message("online") != ESP_OK) {
        ESP_LOGW(TAG, "online Telegram message failed");
    }

    ESP_ERROR_CHECK(audio_board_init());
    ESP_ERROR_CHECK(wake_word_init());
    ESP_LOGI(TAG, "ready for wake word");

    while (true) {
        esp_err_t err = wake_word_wait();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wake wait failed: %s", esp_err_to_name(err));
            continue;
        }
        voice_flow_handle_wake();
    }
}
