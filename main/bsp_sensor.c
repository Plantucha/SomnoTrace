/*
 * SomnoTrace - Touch (CST816) and IMU (QMI8658) sensor driver for wake events
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "bsp_sensor.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "device_settings.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_sensor";

/* ── Hardware Pins (Waveshare ESP32-S3-Touch-LCD-1.54) ────────────── */
#define BSP_PIN_TP_RST      47
#define BSP_PIN_TP_INT      48
#define BSP_PIN_IMU_INT     6

/* ── I2C Addresses ────────────────────────────────────────────────── */
#define CST816_ADDR         0x15
#define QMI8658_ADDR_PRI    0x6B
#define QMI8658_ADDR_SEC    0x6A

/* ── QMI8658 Registers ────────────────────────────────────────────── */
#define QMI8658_REG_WHO_AM_I    0x00
#define QMI8658_REG_CTRL1       0x02
#define QMI8658_REG_CTRL2       0x03
#define QMI8658_REG_CTRL3       0x04
#define QMI8658_REG_CTRL7       0x08
#define QMI8658_REG_CTRL8       0x09
#define QMI8658_REG_CTRL9       0x0A
#define QMI8658_REG_CAL1_L      0x0B
#define QMI8658_REG_CAL1_H      0x0C
#define QMI8658_REG_STATUSINT   0x2F
#define QMI8658_REG_RESET       0x60

#define NOTIF_TOUCH   (1 << 0)
#define NOTIF_MOTION  (1 << 1)

static bool s_has_touch = false;
static bool s_has_imu = false;
static TaskHandle_t s_sensor_task = NULL;
static i2c_master_dev_handle_t s_cst816_dev = NULL;
static i2c_master_dev_handle_t s_qmi8658_dev = NULL;

static void IRAM_ATTR touch_gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t hp_woken = pdFALSE;
    if (s_sensor_task) {
        xTaskNotifyFromISR(s_sensor_task, NOTIF_TOUCH, eSetBits, &hp_woken);
    }
    if (hp_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR imu_gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t hp_woken = pdFALSE;
    if (s_sensor_task) {
        xTaskNotifyFromISR(s_sensor_task, NOTIF_MOTION, eSetBits, &hp_woken);
    }
    if (hp_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t qmi8658_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_qmi8658_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_qmi8658_dev, buf, sizeof(buf), 100);
}

static esp_err_t qmi8658_read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_qmi8658_dev || !val) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_qmi8658_dev, &reg, 1, val, 1, 100);
}

static void cst816_reset_pulse(void)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << BSP_PIN_TP_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);

    gpio_set_level(BSP_PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void sensor_worker_task(void *arg)
{
    (void)arg;
    uint32_t notif = 0;
    int64_t last_wake_ms = 0;

    ESP_LOGI(TAG, "sensor worker task running (touch=%d, imu=%d)",
             s_has_touch, s_has_imu);

    while (1) {
        if (xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY) == pdTRUE) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            const device_settings_t *dev = device_settings_get();
            bool trigger_wake = false;

            if ((notif & NOTIF_TOUCH) && s_has_touch) {
                /* Acknowledge CST816 by reading touch coordinates */
                uint8_t reg = 0x00;
                uint8_t data[7] = {0};
                if (s_cst816_dev) {
                    i2c_master_transmit_receive(s_cst816_dev, &reg, 1, data, sizeof(data), 50);
                }

                if (dev->wake_on_touch) {
                    if (now_ms - last_wake_ms >= 500) {
                        ESP_LOGD(TAG, "touch wake event");
                        trigger_wake = true;
                    }
                }
            }

            if ((notif & NOTIF_MOTION) && s_has_imu) {
                /* Acknowledge QMI8658 by reading interrupt status */
                uint8_t status = 0;
                qmi8658_read_reg(QMI8658_REG_STATUSINT, &status);

                if (dev->wake_on_motion) {
                    if (now_ms - last_wake_ms >= 500) {
                        ESP_LOGD(TAG, "motion wake event (status=0x%02X)", status);
                        trigger_wake = true;
                    }
                }
            }

            if (trigger_wake && dev->wake_timeout_sec > 0) {
                last_wake_ms = now_ms;
                bsp_display_wake_temporary(dev->wake_timeout_sec);
            }
        }
    }
}

esp_err_t bsp_sensor_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_bus_handle();
    if (!bus) {
        ESP_LOGE(TAG, "cannot initialize sensors: I2C bus unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_err));
        return isr_err;
    }

    /* ── 1. Initialize CST816 (Capacitive Touch) ─────────────────── */
    cst816_reset_pulse();
    esp_err_t probe_tp = i2c_master_probe(bus, CST816_ADDR, 100);
    if (probe_tp == ESP_OK) {
        i2c_device_config_t tp_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CST816_ADDR,
            .scl_speed_hz = BSP_I2C_FREQ_HZ,
        };
        if (i2c_master_bus_add_device(bus, &tp_cfg, &s_cst816_dev) == ESP_OK) {
            gpio_config_t int_cfg = {
                .pin_bit_mask = (1ULL << BSP_PIN_TP_INT),
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_NEGEDGE,
            };
            gpio_config(&int_cfg);
            gpio_isr_handler_add(BSP_PIN_TP_INT, touch_gpio_isr_handler, NULL);
            s_has_touch = true;
            ESP_LOGI(TAG, "CST816 touch controller detected on I2C 0x%02X", CST816_ADDR);
        }
    } else {
        ESP_LOGW(TAG, "CST816 touch controller not detected (%s)", esp_err_to_name(probe_tp));
    }

    /* ── 2. Initialize QMI8658 (6-axis IMU) ───────────────────────── */
    uint8_t imu_addr = 0;
    if (i2c_master_probe(bus, QMI8658_ADDR_PRI, 100) == ESP_OK) {
        imu_addr = QMI8658_ADDR_PRI;
    } else if (i2c_master_probe(bus, QMI8658_ADDR_SEC, 100) == ESP_OK) {
        imu_addr = QMI8658_ADDR_SEC;
    }

    if (imu_addr != 0) {
        i2c_device_config_t imu_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = imu_addr,
            .scl_speed_hz = BSP_I2C_FREQ_HZ,
        };
        if (i2c_master_bus_add_device(bus, &imu_cfg, &s_qmi8658_dev) == ESP_OK) {
            uint8_t who = 0;
            qmi8658_read_reg(QMI8658_REG_WHO_AM_I, &who);
            if (who == 0x05) {
                /* Reset chip */
                qmi8658_write_reg(QMI8658_REG_RESET, 0xB0);
                vTaskDelay(pdMS_TO_TICKS(15));

                /* Enable auto-increment, disable sensors during config */
                qmi8658_write_reg(QMI8658_REG_CTRL1, 0x60);
                qmi8658_write_reg(QMI8658_REG_CTRL7, 0x00);

                /* Accel: ±4g range, 50Hz ODR (low noise) */
                qmi8658_write_reg(QMI8658_REG_CTRL2, 0x23);
                /* Gyro: disabled for power saving */
                qmi8658_write_reg(QMI8658_REG_CTRL3, 0x00);

                /* Wake on Motion (WoM) configuration:
                 * CAL1_L: threshold = 180 mg (180 = 0xB4)
                 * CAL1_H: route to INT1, 1 sample blanking */
                qmi8658_write_reg(QMI8658_REG_CAL1_L, 0xB4);
                qmi8658_write_reg(QMI8658_REG_CAL1_H, 0x01);
                qmi8658_write_reg(QMI8658_REG_CTRL9, 0x08); /* Commit WoM command */
                vTaskDelay(pdMS_TO_TICKS(5));

                /* Route AnyMotion to INT1, enable accelerometer */
                qmi8658_write_reg(QMI8658_REG_CTRL8, 0x04);
                qmi8658_write_reg(QMI8658_REG_CTRL7, 0x01);

                gpio_config_t imu_int_cfg = {
                    .pin_bit_mask = (1ULL << BSP_PIN_IMU_INT),
                    .mode = GPIO_MODE_INPUT,
                    .pull_up_en = GPIO_PULLUP_ENABLE,
                    .pull_down_en = GPIO_PULLDOWN_DISABLE,
                    .intr_type = GPIO_INTR_ANYEDGE,
                };
                gpio_config(&imu_int_cfg);
                gpio_isr_handler_add(BSP_PIN_IMU_INT, imu_gpio_isr_handler, NULL);

                s_has_imu = true;
                ESP_LOGI(TAG, "QMI8658 IMU detected on I2C 0x%02X (WHO_AM_I=0x%02X)",
                         imu_addr, who);
            } else {
                ESP_LOGW(TAG, "QMI8658 probed on 0x%02X but WHO_AM_I was 0x%02X (expected 0x05)",
                         imu_addr, who);
            }
        }
    } else {
        ESP_LOGW(TAG, "QMI8658 IMU not detected on 0x6B or 0x6A");
    }

    /* Start listener task if at least one sensor was found */
    if (s_has_touch || s_has_imu) {
        xTaskCreate(sensor_worker_task, "bsp_sensor", 3072, NULL, 5, &s_sensor_task);
    }

    return ESP_OK;
}

bool bsp_sensor_has_touch(void)
{
    return s_has_touch;
}

bool bsp_sensor_has_imu(void)
{
    return s_has_imu;
}
