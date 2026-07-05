/*
 * SomnoTrace - Battery power latch and power button control
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#include "bsp_power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "bsp_display.h"
#include "as11_ble.h"
#include "esp_sleep.h"

#define BSP_PIN_BAT_EN  2
#define BSP_PIN_KEY_PWR 5
#define BSP_PIN_BOOT    0
#define BSP_PIN_KEY_PLUS 4

static const char *TAG = "bsp_power";

void bsp_power_hold(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_BAT_EN,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BSP_PIN_BAT_EN, 1);
    ESP_LOGI(TAG, "battery power latched (BAT_EN=IO%d high)", BSP_PIN_BAT_EN);
}

void bsp_power_off(void)
{
    ESP_LOGW(TAG, "releasing battery latch (power off)");
    
    // Turn off screen backlight
    bsp_display_set_backlight(false);

    // Release battery enable latch
    gpio_set_level(BSP_PIN_BAT_EN, 0);

    // Wait until PWR button is released to prevent immediate wakeup if on USB
    while (gpio_get_level(BSP_PIN_KEY_PWR) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Wait 2 seconds for power to cut out (if on battery)
    vTaskDelay(pdMS_TO_TICKS(2000));

    // If still running, we are powered via USB-C. Enter deep sleep.
    ESP_LOGW(TAG, "Still powered. Entering deep sleep...");
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_5, 0); // wakeup on GPIO5 low
    esp_deep_sleep_start();
}

static void power_button_task(void *arg)
{
    const int hold_ms = (int)(intptr_t)arg;

    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_KEY_PWR,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    const int poll_ms = 50;
    int held_ms = 0;
    while (true) {
        if (gpio_get_level(BSP_PIN_KEY_PWR) == 0) {
            held_ms += poll_ms;
            if (held_ms >= hold_ms) {
                ESP_LOGW(TAG, "power button long-press: shutting down");
                const char *msg[] = { "Powering off..." };
                bsp_display_show_lines("SomnoTrace", msg, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                bsp_power_off();
                while (gpio_get_level(BSP_PIN_KEY_PWR) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(poll_ms));
                }
                held_ms = 0;
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void bsp_power_start_button_monitor(int hold_ms)
{
    xTaskCreate(power_button_task, "pwr_btn", 2560,
                (void *)(intptr_t)hold_ms, 5, NULL);
}

static void boot_button_task(void *arg)
{
    volatile bool *flag = (volatile bool *)arg;

    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_BOOT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    const int poll_ms = 50;
    int held_ms = 0;
    while (true) {
        if (gpio_get_level(BSP_PIN_BOOT) == 0) {
            held_ms += poll_ms;
            if (held_ms >= 5000 && !*flag) {
                ESP_LOGW(TAG, "BOOT long-press: flagging SoftAP entry");
                const char *msg[] = { "Entering Wi-Fi setup..." };
                bsp_display_show_lines("SomnoTrace", msg, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                *flag = true;
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void bsp_power_start_boot_monitor(volatile bool *softap_flag, int hold_ms)
{
    (void)hold_ms; /* use fixed 5 s for BOOT; arg reserved for future */
    xTaskCreate(boot_button_task, "boot_btn", 2560,
                (void *)softap_flag, 5, NULL);
}

/* ── PLUS button (IO4) — double-click to stop therapy ─────────────── */

static void plus_button_task(void *arg)
{
    (void)arg;

    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_KEY_PLUS,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    const int poll_ms = 50;
    const int double_click_window_ms = 400;
    int last_press_ms = -1;

    while (true) {
        if (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
            int now_ms = (int)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            if (last_press_ms >= 0 &&
                (now_ms - last_press_ms) < double_click_window_ms) {
                /* Double-click detected */
                ESP_LOGI(TAG, "PLUS button double-click");

                if (bsp_display_is_therapy_active()) {
                    ESP_LOGI(TAG, "stopping therapy via EnterStandby RPC");
                    esp_err_t ret = as11_ble_stop_therapy();
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "stop_therapy failed: %s",
                                 esp_err_to_name(ret));
                    }
                } else {
                    ESP_LOGD(TAG, "PLUS double-click: therapy not active, ignoring");
                }

                last_press_ms = -1;  /* reset */
            } else {
                last_press_ms = now_ms;
            }

            /* Wait for button release */
            while (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
                vTaskDelay(pdMS_TO_TICKS(poll_ms));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void bsp_power_start_plus_monitor(void)
{
    xTaskCreate(plus_button_task, "plus_btn", 3072, NULL, 5, NULL);
}
