#include "led_ring.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RMT_RESOLUTION_HZ 10000000
#define LED_BYTES_PER_PIXEL 3

static const char *TAG = "led_ring";
static uint8_t pixels[LED_COUNT * LED_BYTES_PER_PIXEL];
static rmt_channel_handle_t led_channel;
static rmt_encoder_handle_t led_encoder;
static rmt_transmit_config_t tx_config = {.loop_count = 0};
static uint8_t chase_index;

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    rmt_symbol_word_t reset_code;
    int state;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws2812 = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws2812->state) {
    case 0:
        encoded_symbols += ws2812->bytes_encoder->encode(ws2812->bytes_encoder, channel, data,
                                                         data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws2812->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            break;
        }
        __attribute__((fallthrough));
    case 1:
        encoded_symbols += ws2812->copy_encoder->encode(ws2812->copy_encoder, channel,
                                                        &ws2812->reset_code,
                                                        sizeof(ws2812->reset_code),
                                                        &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws2812->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
    }

    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ws2812_delete(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812 = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws2812->bytes_encoder);
    rmt_del_encoder(ws2812->copy_encoder);
    free(ws2812);
    return ESP_OK;
}

static esp_err_t ws2812_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812 = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws2812->bytes_encoder);
    rmt_encoder_reset(ws2812->copy_encoder);
    ws2812->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t ws2812_new_encoder(rmt_encoder_handle_t *ret_encoder)
{
    ws2812_encoder_t *ws2812 = rmt_alloc_encoder_mem(sizeof(ws2812_encoder_t));
    ESP_RETURN_ON_FALSE(ws2812, ESP_ERR_NO_MEM, TAG, "no encoder memory");

    ws2812->base.encode = ws2812_encode;
    ws2812->base.del = ws2812_delete;
    ws2812->base.reset = ws2812_reset;

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {.level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 8},
        .bit1 = {.level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 4},
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&bytes_config, &ws2812->bytes_encoder), TAG,
                        "bytes encoder");

    rmt_copy_encoder_config_t copy_config = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy_config, &ws2812->copy_encoder), TAG,
                        "copy encoder");

    const uint32_t reset_ticks = RMT_RESOLUTION_HZ / 1000000 * 300 / 2;
    ws2812->reset_code = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };

    *ret_encoder = &ws2812->base;
    return ESP_OK;
}

static esp_err_t show(void)
{
    ESP_RETURN_ON_FALSE(led_channel && led_encoder, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(rmt_transmit(led_channel, led_encoder, pixels, sizeof(pixels), &tx_config),
                        TAG, "rmt transmit");
    return rmt_tx_wait_all_done(led_channel, portMAX_DELAY);
}

esp_err_t led_ring_init(void)
{
    if (led_channel) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t channel_config = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &led_channel), TAG, "new tx channel");
    ESP_RETURN_ON_ERROR(ws2812_new_encoder(&led_encoder), TAG, "new encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(led_channel), TAG, "enable channel");
    return led_ring_clear();
}

esp_err_t led_ring_clear(void)
{
    memset(pixels, 0, sizeof(pixels));
    return show();
}

esp_err_t led_ring_set(uint8_t red, uint8_t green, uint8_t blue)
{
    red = (uint8_t)(((uint16_t)red * LED_BRIGHTNESS) / 255);
    green = (uint8_t)(((uint16_t)green * LED_BRIGHTNESS) / 255);
    blue = (uint8_t)(((uint16_t)blue * LED_BRIGHTNESS) / 255);

    for (int led = 0; led < LED_COUNT; led++) {
        pixels[led * 3 + 0] = red;
        pixels[led * 3 + 1] = green;
        pixels[led * 3 + 2] = blue;
    }
    return show();
}

esp_err_t led_ring_flash(uint8_t red, uint8_t green, uint8_t blue, uint32_t on_ms,
                         uint32_t off_ms, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        ESP_RETURN_ON_ERROR(led_ring_set(red, green, blue), TAG, "flash on");
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        ESP_RETURN_ON_ERROR(led_ring_clear(), TAG, "flash off");
        if (off_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
    return ESP_OK;
}

esp_err_t led_ring_chase_step(uint8_t red, uint8_t green, uint8_t blue)
{
    red = (uint8_t)(((uint16_t)red * LED_BRIGHTNESS) / 255);
    green = (uint8_t)(((uint16_t)green * LED_BRIGHTNESS) / 255);
    blue = (uint8_t)(((uint16_t)blue * LED_BRIGHTNESS) / 255);

    memset(pixels, 0, sizeof(pixels));
    pixels[chase_index * 3 + 0] = red;
    pixels[chase_index * 3 + 1] = green;
    pixels[chase_index * 3 + 2] = blue;
    chase_index = (uint8_t)((chase_index + 1) % LED_COUNT);
    return show();
}
