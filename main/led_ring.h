#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t led_ring_init(void);
esp_err_t led_ring_clear(void);
esp_err_t led_ring_set(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t led_ring_flash(uint8_t red, uint8_t green, uint8_t blue, uint32_t on_ms,
                         uint32_t off_ms, uint8_t count);
esp_err_t led_ring_chase_step(uint8_t red, uint8_t green, uint8_t blue);
