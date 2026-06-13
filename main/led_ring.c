#include "led_ring.h"

#include "app_config.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#define RMT_RESOLUTION_HZ 10000000

static const char *TAG = "led_ring";
static led_strip_handle_t led_strip;
static uint8_t chase_index;

static void scale_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
    *red = (uint8_t)(((uint16_t)*red * LED_BRIGHTNESS) / 255);
    *green = (uint8_t)(((uint16_t)*green * LED_BRIGHTNESS) / 255);
    *blue = (uint8_t)(((uint16_t)*blue * LED_BRIGHTNESS) / 255);
}

static esp_err_t show(void)
{
    ESP_RETURN_ON_FALSE(led_strip, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return led_strip_refresh(led_strip);
}

esp_err_t led_ring_init(void)
{
    if (led_strip) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip), TAG,
                        "new led strip");
    return led_ring_clear();
}

esp_err_t led_ring_clear(void)
{
    ESP_RETURN_ON_FALSE(led_strip, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    for (int led = 0; led < LED_COUNT; led++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(led_strip, led, 0, 0, 0), TAG, "clear pixel");
    }
    return show();
}

esp_err_t led_ring_set(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(led_strip, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    scale_color(&red, &green, &blue);

    for (int led = 0; led < LED_COUNT; led++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(led_strip, led, red, green, blue), TAG,
                            "set pixel");
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
    ESP_RETURN_ON_FALSE(led_strip, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    scale_color(&red, &green, &blue);

    for (int led = 0; led < LED_COUNT; led++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(led_strip, led, 0, 0, 0), TAG, "clear pixel");
    }
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(led_strip, chase_index, red, green, blue), TAG,
                        "set chase pixel");
    chase_index = (uint8_t)((chase_index + 1) % LED_COUNT);
    return show();
}
