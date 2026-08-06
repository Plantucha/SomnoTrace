/*
 * SomnoTrace - Battery power latch and power button control
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#include "bsp_power.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "bsp_display.h"
#include "as11_ble.h"
#include "esp_sleep.h"
#include "psram_task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_writer.h"
#include "therapy_alert.h"

#define BSP_PIN_BAT_EN   2
#define BSP_PIN_KEY_PWR  5
#define BSP_PIN_BOOT     0
#define BSP_PIN_KEY_PLUS 4
#define BSP_PIN_BAT_ADC  1
#define BSP_PIN_CHG_STAT 3

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

        /* --- PLUS button: double-click = stop therapy, single-click = alert ack --- */
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
                /* First press: record timestamp and wait for release. */
                s_btn.plus_last_press_ms = now_ms;
            }

            /* Wait for button release. */
            while (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
                vTaskDelay(pdMS_TO_TICKS(poll_ms));
            }

            /* If this was a single press (not consumed by double-click),
             * wait for the double-click window to expire, then fire
             * single-click action (alert acknowledgement). */
            if (s_btn.plus_last_press_ms >= 0) {
                int release_ms = (int)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                int remaining = double_click_window_ms -
                                (release_ms - s_btn.plus_last_press_ms);
                if (remaining > 0) {
                    vTaskDelay(pdMS_TO_TICKS(remaining));
                }
                /* If no second press arrived during the window, resolve as single-click. */
                if (s_btn.plus_last_press_ms >= 0 &&
                    gpio_get_level(BSP_PIN_KEY_PLUS) == 1) {
                    ESP_LOGI(TAG, "PLUS button single-click — alert acknowledge");
                    therapy_alert_acknowledge();
                    s_btn.plus_last_press_ms = -1;
                }
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

/* ── Battery monitoring ───────────────────────────────────────────────
 * GPIO1 (BAT_ADC) has a 200k/100k voltage divider: VBAT = VADC × 3.
 * The ESP32-S3 ADC1 channel 0 maps to GPIO1.  GPIO3 (CHG_STAT) is low
 * while charging.
 *
 * A single monitor task owns the ADC and publishes a snapshot; no
 * consumer ever triggers a conversion.  Three things matter for a
 * usable reading:
 *
 *   1. Window width, not sample count.  Wi-Fi/BLE TX bursts dip the
 *      rail for a few hundred microseconds.  Samples taken back-to-back
 *      all land inside one such dip and average to the same wrong
 *      answer, so the burst is deliberately spread over ~1 s.  A
 *      trimmed mean then discards the dip tails outright rather than
 *      averaging them in.
 *
 *   2. Per-chip calibration.  ADC_ATTEN_DB_12 is not a clean 0-3.3 V
 *      range and varies between chips, so we use the eFuse-backed
 *      calibration scheme where available.  Assuming a nominal
 *      full-scale reads several percent low, which is what forced the
 *      old code to fudge its 100% endpoint down to 3930 mV.
 *
 *   3. Li-ion voltage is not linear in charge.  The curve is flat from
 *      ~3.9 V to ~3.6 V (most of the capacity) then falls off a cliff,
 *      so percent comes from an OCV table, not a straight line.
 *
 * Charging raises the terminal voltage above the cell's true OCV, so
 * plugging in genuinely steps the reading upward — no filter can remove
 * that.  It is handled at the presentation layer instead: an IR-drop
 * offset while charging, a monotonic-while-charging rule, and a slew
 * limit on the published percentage that turns any residual step into a
 * smooth walk. */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali = NULL;
static bool s_adc_ready = false;
static const adc_channel_t s_bat_adc_channel = ADC_CHANNEL_0;  /* GPIO1 = ADC1_CH0 */

#define BAT_DIVIDER_RATIO   3     /* 200k + 100k divider → ×3 */

/* Sample burst: 256 reads spread over ~1 s (4 ms apart). */
#define BAT_BURST_SAMPLES   256
#define BAT_BURST_GAP_MS    4
/* Discard the lowest and highest eighth before averaging. */
#define BAT_TRIM_FRACTION   8

/* Cadence: a real battery cannot move fast, so sample rarely. */
#define BAT_PERIOD_DISCHARGE_S  60
#define BAT_PERIOD_CHARGE_S     20   /* users watch a charge bar */
#define BAT_UNPLUG_SETTLE_S     30   /* let terminal voltage relax to OCV */

/* Terminal voltage sits above OCV by roughly this much while charging. */
#define BAT_CHARGE_IR_OFFSET_MV 120

/* Above this voltage the reading cannot be a real Li-ion cell (max ~4.2 V).
 * USB power with no battery installed pegs the ADC at ~5 V through the
 * divider.  Use this to detect battery absence and hide the indicator. */
#define BAT_NO_BATTERY_MV      4250

/* Published snapshot, guarded by a mutex. */
static SemaphoreHandle_t s_bat_mutex = NULL;
static bsp_battery_t s_bat_state = {
    .percent = -1, .millivolts = -1, .charging = false, .valid = false, .age_s = 0,
};
static TickType_t s_bat_last_ok_tick = 0;

/* Li-ion open-circuit voltage → percent.  Descending by mV; linear
 * interpolation between rows.  The 100% anchor is adaptive: it learns the
 * cell's charge-termination voltage so the display reaches 100% when the
 * charger IC stops, rather than requiring an unrealistic 4200 mV. */
static const struct { int mv; int pct; } s_ocv_curve[] = {
    { 4200, 100 },
    { 4100,  90 },
    { 3950,  75 },
    { 3850,  60 },
    { 3750,  45 },
    { 3650,  30 },
    { 3550,  15 },
    { 3450,   5 },
    { 3300,   0 },
};

/* Adaptive 100% anchor: updated to the observed charge-termination voltage.
 * Defaults to the OCV curve's 4200 mV; replaced with the real value once the
 * charger IC stops for the first time.  Persisted in NVS so it survives reboots. */
#define BAT_NVS_NS         "bat"
#define BAT_NVS_KEY_FULL   "full_mv"
#define BAT_ANCHOR_MIN_MV  4000
#define BAT_ANCHOR_MAX_MV  4200
static int s_full_charge_mv = 4200;

/* NVS write callback — runs on the internal-stack nvs_writer task. */
static esp_err_t do_save_bat_anchor(void *arg)
{
    int mv = *(const int *)arg;
    nvs_handle_t h;
    if (nvs_open(BAT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t err = nvs_set_i32(h, BAT_NVS_KEY_FULL, mv);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Load the persisted anchor at boot.  Called before the monitor task starts
 * (internal stack at that point), so a direct nvs_open is safe. */
static void bat_load_anchor(void)
{
    nvs_handle_t h;
    if (nvs_open(BAT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t mv = 4200;
    if (nvs_get_i32(h, BAT_NVS_KEY_FULL, &mv) == ESP_OK &&
        mv >= BAT_ANCHOR_MIN_MV && mv <= BAT_ANCHOR_MAX_MV) {
        s_full_charge_mv = mv;
        ESP_LOGI(TAG, "battery: loaded full-charge anchor = %dmV from NVS", mv);
    }
    nvs_close(h);
}

static int bat_mv_to_percent(int mv)
{
    const int n = sizeof(s_ocv_curve) / sizeof(s_ocv_curve[0]);
    if (mv >= s_full_charge_mv) return 100;
    if (mv <= s_ocv_curve[n - 1].mv) return 0;
    for (int i = 0; i < n - 1; i++) {
        int hi_mv = s_ocv_curve[i].mv, lo_mv = s_ocv_curve[i + 1].mv;
        int hi_pct = s_ocv_curve[i].pct, lo_pct = s_ocv_curve[i + 1].pct;
        /* Stretch the top segment so its upper bound is the adaptive 100%
         * anchor rather than the hardcoded 4200 mV. */
        if (i == 0) hi_mv = s_full_charge_mv;
        if (mv <= hi_mv && mv > lo_mv) {
            return lo_pct + (mv - lo_mv) * (hi_pct - lo_pct) / (hi_mv - lo_mv);
        }
    }
    return 0;
}

static esp_err_t bat_adc_init(void)
{
    if (s_adc_ready) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery: ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_bat_adc_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery: ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Per-chip calibration from eFuse.  Without it every unit reads a few
     * percent off, which is the "heaps of variations" problem. */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_bat_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        ESP_LOGI(TAG, "battery: using eFuse ADC calibration");
    } else {
        s_adc_cali = NULL;
        ESP_LOGW(TAG, "battery: no eFuse ADC calibration, falling back to "
                      "nominal scaling (readings may be a few %% off)");
    }

    s_adc_ready = true;
    return ESP_OK;
}

static int bat_raw_to_mv(int raw)
{
    if (s_adc_cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
            return mv * BAT_DIVIDER_RATIO;
        }
    }
    /* Uncalibrated fallback: nominal 0-3100 mV full scale for DB_12. */
    return (raw * 3100 * BAT_DIVIDER_RATIO) / 4095;
}

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

/* One burst: BAT_BURST_SAMPLES reads spread over the sampling window,
 * combined with a trimmed mean.  Returns VBAT in mV, or -1 on failure. */
static int bat_sample_burst(void)
{
    static int samples[BAT_BURST_SAMPLES];
    int n = 0;

    for (int i = 0; i < BAT_BURST_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_bat_adc_channel, &raw) == ESP_OK) {
            samples[n++] = raw;
        }
        vTaskDelay(pdMS_TO_TICKS(BAT_BURST_GAP_MS));
    }
    if (n == 0) return -1;

    qsort(samples, n, sizeof(int), cmp_int);

    /* Trimmed mean: drop the tails where the TX dips and spikes live. */
    int trim = n / BAT_TRIM_FRACTION;
    int lo = trim, hi = n - trim;
    if (hi <= lo) { lo = 0; hi = n; }

    int64_t sum = 0;
    for (int i = lo; i < hi; i++) sum += samples[i];
    int raw_avg = (int)(sum / (hi - lo));

    return bat_raw_to_mv(raw_avg);
}

static void bat_publish(int mv, int pct, bool charging, bool ok)
{
    if (!s_bat_mutex) return;
    xSemaphoreTake(s_bat_mutex, portMAX_DELAY);
    if (ok) {
        s_bat_state.millivolts = mv;
        s_bat_state.percent = pct;
        s_bat_state.valid = true;
        s_bat_last_ok_tick = xTaskGetTickCount();
    } else {
        s_bat_state.millivolts = mv;
        s_bat_state.percent = pct;
        s_bat_state.valid = false;
    }
    s_bat_state.charging = charging;
    xSemaphoreGive(s_bat_mutex);
}

static void battery_monitor_task(void *arg)
{
    (void)arg;

    if (bat_adc_init() != ESP_OK) {
        ESP_LOGE(TAG, "battery: monitor exiting, ADC unavailable");
        vTaskDelete(NULL);
        return;
    }

    int  filtered_mv = -1;   /* IIR-filtered VBAT */
    int  shown_pct = -1;     /* slew-limited percentage actually published */
    bool prev_charging = bsp_power_is_charging();
    bool settling = false;
    TickType_t settle_until = 0;

    for (;;) {
        bool charging = bsp_power_is_charging();

        /* Charger plugged/unplugged: react at once rather than waiting out
         * the cadence, and after unplug give the cell time to relax before
         * trusting the reading. */
        if (charging != prev_charging) {
            ESP_LOGI(TAG, "battery: charger %s",
                     charging ? "connected" : "disconnected");
            if (!charging) {
                settling = true;
                settle_until = xTaskGetTickCount() +
                               pdMS_TO_TICKS(BAT_UNPLUG_SETTLE_S * 1000);
                /* Record the charge-termination voltage as the 100% anchor.
                 * filtered_mv is the IR-compensated OCV estimate from during
                 * charging.  The settled OCV after unplug will be slightly
                 * higher, so the display reaches 100%.  Persist to NVS so it
                 * survives reboots. */
                if (filtered_mv >= BAT_ANCHOR_MIN_MV && filtered_mv <= BAT_ANCHOR_MAX_MV) {
                    if (filtered_mv != s_full_charge_mv) {
                        s_full_charge_mv = filtered_mv;
                        ESP_LOGI(TAG, "battery: full-charge anchor = %dmV",
                                 s_full_charge_mv);
                        nvs_writer_run(do_save_bat_anchor, &s_full_charge_mv);
                    }
                }
            }
            /* Reset the IIR filter and displayed percentage on any charger
             * transition so they adopt the next sample directly.  Without
             * this the filter lags ~4 cycles (IR offset change) and the
             * slew-limited percentage takes minutes to reach the new target. */
            filtered_mv = -1;
            shown_pct = -1;
            prev_charging = charging;
        }

        int mv = bat_sample_burst();
        if (mv > 0) {
            /* No battery installed: USB rail drives the divider to ~5 V,
             * well above any real Li-ion cell.  Publish invalid state so
             * the LCD and /api/status hide the battery indicator. */
            if (mv > BAT_NO_BATTERY_MV) {
                filtered_mv = -1;
                shown_pct = -1;
                bat_publish(mv, -1, false, false);
                ESP_LOGD(TAG, "battery: no battery detected (%dmV > %dmV)",
                         mv, BAT_NO_BATTERY_MV);
            } else {
                /* Compensate the charging IR rise so the charge and discharge
                 * curves roughly agree. */
                int ocv_mv = charging ? mv - BAT_CHARGE_IR_OFFSET_MV : mv;

                /* IIR low-pass across bursts.  Anything faster than this is
                 * noise by definition at a 20-60 s cadence. */
                if (filtered_mv < 0) filtered_mv = ocv_mv;
                else filtered_mv += (ocv_mv - filtered_mv) / 4;

                int target_pct = bat_mv_to_percent(filtered_mv);

                if (shown_pct < 0) {
                    shown_pct = target_pct;      /* first reading: adopt directly */
                } else if (settling && xTaskGetTickCount() < settle_until) {
                    /* Hold the displayed value while the cell relaxes. */
                } else {
                    settling = false;
                    /* Slew limit: at most 1 percentage point per update.  This is
                     * what removes the visible "jump" on plug-in. */
                    if (target_pct > shown_pct) shown_pct++;
                    else if (target_pct < shown_pct) shown_pct--;

                    /* While charging the bar must never walk backwards. */
                    if (charging && target_pct < shown_pct) shown_pct++;
                }

                bat_publish(filtered_mv, shown_pct, charging, true);
                ESP_LOGD(TAG, "battery: vbat=%dmV filt=%dmV target=%d%% shown=%d%% chg=%d",
                         mv, filtered_mv, target_pct, shown_pct, charging);
            }
        } else {
            ESP_LOGW(TAG, "battery: sample burst failed");
            bat_publish(0, 0, charging, false);
        }

        int period_s = charging ? BAT_PERIOD_CHARGE_S : BAT_PERIOD_DISCHARGE_S;

        /* Sleep in 1 s slices so a plug/unplug edge is noticed promptly
         * instead of up to a full period late. */
        for (int i = 0; i < period_s; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (bsp_power_is_charging() != prev_charging) break;
        }
    }
}

esp_err_t bsp_power_battery_monitor_start(void)
{
    if (s_bat_mutex) return ESP_OK;   /* already running */

    bat_load_anchor();
    nvs_writer_init();

    s_bat_mutex = xSemaphoreCreateMutex();
    if (!s_bat_mutex) return ESP_ERR_NO_MEM;

    TaskHandle_t h = psram_task_create(battery_monitor_task, "bat_mon", 4096,
                                       NULL, 3, tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        vSemaphoreDelete(s_bat_mutex);
        s_bat_mutex = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "battery monitor started (%ds discharge / %ds charge cadence)",
             BAT_PERIOD_DISCHARGE_S, BAT_PERIOD_CHARGE_S);
    return ESP_OK;
}

void bsp_power_battery_get(bsp_battery_t *out)
{
    if (!out) return;
    if (!s_bat_mutex) {
        out->percent = -1;
        out->millivolts = -1;
        out->charging = false;
        out->valid = false;
        out->age_s = 0;
        return;
    }
    xSemaphoreTake(s_bat_mutex, portMAX_DELAY);
    *out = s_bat_state;
    if (s_bat_state.valid) {
        out->age_s = (uint32_t)((xTaskGetTickCount() - s_bat_last_ok_tick)
                                * portTICK_PERIOD_MS / 1000);
    }
    xSemaphoreGive(s_bat_mutex);
}

int bsp_power_battery_percent(void)
{
    bsp_battery_t b;
    bsp_power_battery_get(&b);
    return b.valid ? b.percent : -1;
}

bool bsp_power_is_charging(void)
{
    /* Configure CHG_STAT as input with pull-up if not already done.
     * The charger IC pulls CHG_STAT low when charging. */
    static bool chg_configured = false;
    if (!chg_configured) {
        gpio_config_t cfg = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << BSP_PIN_CHG_STAT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        chg_configured = true;
    }
    return (gpio_get_level(BSP_PIN_CHG_STAT) == 0);
}
