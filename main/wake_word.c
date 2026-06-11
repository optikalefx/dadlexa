#include "wake_word.h"

#include <stdlib.h>

#include "app_config.h"
#include "audio_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "wake_word";
static const esp_wn_iface_t *wake_net;
static model_iface_data_t *wake_data;
static int chunk_samples;

esp_err_t wake_word_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(models, ESP_FAIL, TAG, "model load failed");

    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, WAKE_MODEL_HINT);
    ESP_RETURN_ON_FALSE(model_name, ESP_FAIL, TAG, "no wake model matched %s", WAKE_MODEL_HINT);

    wake_net = esp_wn_handle_from_name(model_name);
    ESP_RETURN_ON_FALSE(wake_net, ESP_FAIL, TAG, "wake handle failed");
    wake_data = wake_net->create(model_name, DET_MODE_95);
    ESP_RETURN_ON_FALSE(wake_data, ESP_FAIL, TAG, "wake create failed");

    chunk_samples = wake_net->get_samp_chunksize(wake_data);
    ESP_LOGI(TAG, "listening model=%s chunk=%d rate=%d", model_name, chunk_samples,
             wake_net->get_samp_rate(wake_data));
    ESP_RETURN_ON_FALSE(wake_net->get_samp_rate(wake_data) == RECORD_SAMPLE_RATE, ESP_FAIL,
                        TAG, "unexpected wake sample rate");
    return ESP_OK;
}

esp_err_t wake_word_wait(void)
{
    ESP_RETURN_ON_FALSE(wake_net && wake_data && chunk_samples > 0, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");
    int16_t *mono = malloc(chunk_samples * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(mono, ESP_ERR_NO_MEM, TAG, "wake buffer");

    while (true) {
        audio_board_lock();
        esp_err_t mode_err = audio_board_prepare_recording();
        if (mode_err != ESP_OK) {
            audio_board_unlock();
            free(mono);
            return mode_err;
        }
        esp_err_t err = audio_board_read_mono_gain(mono, chunk_samples, MIC_SOFTWARE_GAIN);
        audio_board_unlock();
        if (err != ESP_OK) {
            free(mono);
            return err;
        }

        wakenet_state_t state = wake_net->detect(wake_data, mono);
        if (state == WAKENET_DETECTED || state > 0) {
            ESP_LOGI(TAG, "detected");
            free(mono);
            return ESP_OK;
        }
    }
}
