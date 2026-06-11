#include "micro_opus_player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "micro_opus/ogg_opus_decoder.h"

static const char *TAG = "opus_player";

extern "C" esp_err_t play_micro_opus_ogg(const uint8_t *ogg_data, size_t ogg_size)
{
    ESP_RETURN_ON_FALSE(ogg_data && ogg_size > 0, ESP_ERR_INVALID_ARG, TAG, "empty ogg");

    micro_opus::OggOpusDecoder decoder(false, 48000, 1);
    std::vector<int16_t> pcm_buffer(960);

    const uint8_t *input = ogg_data;
    size_t remaining = ogg_size;
    size_t total_pcm = 0;

    while (remaining > 0) {
        size_t consumed = 0;
        size_t decoded = 0;
        micro_opus::OggOpusResult result = decoder.decode(
            input, remaining, reinterpret_cast<uint8_t *>(pcm_buffer.data()),
            pcm_buffer.size() * sizeof(int16_t), consumed, decoded);

        if (result == micro_opus::OGG_OPUS_OUTPUT_BUFFER_TOO_SMALL) {
            size_t required = decoder.get_required_output_buffer_size() / sizeof(int16_t);
            pcm_buffer.resize(std::max<size_t>(required, 960));
            continue;
        }
        ESP_RETURN_ON_FALSE(result == micro_opus::OGG_OPUS_OK, ESP_FAIL, TAG, "decode failed");

        if (consumed > 0) {
            input += consumed;
            remaining -= consumed;
        }
        if (decoded > 0) {
            uint8_t channels = decoder.get_channels();
            if (channels == 0) {
                channels = 1;
            }
            size_t samples = decoded * channels;
            ESP_RETURN_ON_ERROR(audio_board_write_pcm(pcm_buffer.data(), samples), TAG, "pcm write");
            total_pcm += samples;
        }
        ESP_RETURN_ON_FALSE(consumed > 0 || decoded > 0, ESP_FAIL, TAG, "decode stalled");
        taskYIELD();
    }

    return total_pcm > 0 ? ESP_OK : ESP_FAIL;
}
