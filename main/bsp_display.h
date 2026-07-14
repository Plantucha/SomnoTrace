/*
 * SomnoTrace - ST7789 LCD driver
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t bsp_display_init(void);
void bsp_display_show_number(uint32_t value);
void bsp_display_show_lines(const char *title, const char *const *lines, int n_lines);
void bsp_display_set_wifi_connected(bool connected);
void bsp_display_set_as11_paired(bool paired);

/* Therapy graph mode */
void bsp_display_set_therapy_active(bool active);
void bsp_display_push_flow(float flow_lpm);
bool bsp_display_is_therapy_active(void);

/* Backlight control (LEDC PWM on GPIO 46).
 * set_brightness: 1-200 (tenth-percent units: 1=0.1%, 200=20.0%), applied immediately.
 * set_backlight: hard on/off (used for therapy LCD-off mode).
 * get_brightness: returns last set brightness value. */
void bsp_display_set_brightness(uint8_t percent);
void bsp_display_set_backlight(bool on);
uint8_t bsp_display_get_brightness(void);
