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
#include "psram_task.h"

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

/* ── Consolidated button monitor task ────────────────────────────────
 * Polls PWR (IO5), BOOT (IO0), and PLUS (IO4) in a single task.
 * Replaces three separate tasks, saving ~5KB internal RAM stack + 2 TCBs.
 */

static struct {
    int  pwr_hold_ms;
    int  pwr_held_ms;
    volatile bool *softap_flag;
    int  boot_held_ms;
    int  plus_last_press_ms;
} s_btn;

static void button_monitor_task(void *arg)
{
    (void)arg;
    const int poll_ms = 50;
    const int double_click_window_ms = 400;

    /* Configure all three button GPIOs at once */
    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BSP_PIN_KEY_PWR) |
                        (1ULL << BSP_PIN_BOOT)    |
                        (1ULL << BSP_PIN_KEY_PLUS),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    while (true) {
        /* --- PWR button: long-press = power off --- */
        if (gpio_get_level(BSP_PIN_KEY_PWR) == 0) {
            s_btn.pwr_held_ms += poll_ms;
            if (s_btn.pwr_held_ms >= s_btn.pwr_hold_ms) {
                ESP_LOGW(TAG, "power button long-press: shutting down");
                const char *msg[] = { "Powering off..." };
                bsp_display_show_lines("SomnoTrace", msg, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                bsp_power_off();
                while (gpio_get_level(BSP_PIN_KEY_PWR) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(poll_ms));
                }
                s_btn.pwr_held_ms = 0;
            }
        } else {
            s_btn.pwr_held_ms = 0;
        }

        /* --- BOOT button: 5 s hold = SoftAP entry --- */
        if (gpio_get_level(BSP_PIN_BOOT) == 0) {
            s_btn.boot_held_ms += poll_ms;
            if (s_btn.boot_held_ms >= 5000 && s_btn.softap_flag && !*s_btn.softap_flag) {
                ESP_LOGW(TAG, "BOOT long-press: flagging SoftAP entry");
                const char *msg[] = { "Entering Wi-Fi setup..." };
                bsp_display_show_lines("SomnoTrace", msg, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                *s_btn.softap_flag = true;
            }
        } else {
            s_btn.boot_held_ms = 0;
        }

        /* --- PLUS button: double-click = stop therapy --- */
        if (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
            int now_ms = (int)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            if (s_btn.plus_last_press_ms >= 0 &&
                (now_ms - s_btn.plus_last_press_ms) < double_click_window_ms) {
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

                s_btn.plus_last_press_ms = -1;
            } else {
                s_btn.plus_last_press_ms = now_ms;
            }

            while (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
                vTaskDelay(pdMS_TO_TICKS(poll_ms));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void bsp_power_start_button_monitor(int hold_ms)
{
    s_btn.pwr_hold_ms = hold_ms;
    s_btn.pwr_held_ms = 0;
    s_btn.boot_held_ms = 0;
    s_btn.plus_last_press_ms = -1;
    psram_task_create(button_monitor_task, "btn_mon", 3072, NULL, 5, tskNO_AFFINITY, NULL, NULL);
}

void bsp_power_start_boot_monitor(volatile bool *softap_flag, int hold_ms)
{
    (void)hold_ms;
    s_btn.softap_flag = softap_flag;
}

void bsp_power_start_plus_monitor(void)
{
    /* No-op: PLUS button is handled by the consolidated button_monitor_task */
}
