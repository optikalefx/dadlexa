#include "audio_board.h"
#include "esp_log.h"
#include "led_ring.h"
#include "telegram_service.h"
#include "voice_flow.h"
#include "wake_word.h"
#include "web_admin.h"
#include "wifi_service.h"

static const char *TAG = "app";

/* Business-critical orchestrator: boots hardware/network services, announces
 * Telegram availability, then loops forever waiting for wake-word events. */
void app_main(void)
{
    ESP_ERROR_CHECK(led_ring_init());
    ESP_ERROR_CHECK(led_ring_clear());
    ESP_ERROR_CHECK(wifi_service_start());
    ESP_LOGI(TAG, "wifi connected");
    ESP_ERROR_CHECK(web_admin_start());

    if (telegram_send_message("online") != ESP_OK) {
        ESP_LOGW(TAG, "online Telegram message failed");
    }

    ESP_ERROR_CHECK(audio_board_init());
    ESP_ERROR_CHECK(voice_flow_start_button_task());
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
