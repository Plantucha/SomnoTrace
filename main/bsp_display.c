/*
 * SomnoTrace - ST7789 LCD driver
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "bsp_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "font_roboto.h"
#include "esp_wifi.h"

#define LCD_PIN_SCLK        38
#define LCD_PIN_MOSI        39
#define LCD_PIN_DC          45
#define LCD_PIN_CS          21
#define LCD_PIN_RST         40
#define LCD_PIN_BL          46

#define LCD_H_RES           240
#define LCD_V_RES           240
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8
#define LCD_INVERT_COLOR    true

static const char *TAG = "bsp_display";

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;
static bool s_wifi_connected = false;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static inline uint32_t utf8_decode(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    if (!p || !*p) return 0;

    uint32_t c = *p;
    if (c < 0x80) {
        (*s)++;
        return c;
    }

    if ((c & 0xE0) == 0xC0) {
        if (!p[1]) {
            *s += 1;
            return '?';
        }
        c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        *s += 2;
        return c;
    }

    if ((c & 0xF0) == 0xE0) {
        if (!p[1]) {
            *s += 1;
            return '?';
        }
        if (!p[2]) {
            *s += 2;
            return '?';
        }
        c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *s += 3;
        return c;
    }

    if ((c & 0xF8) == 0xF0) {
        if (!p[1]) {
            *s += 1;
            return '?';
        }
        if (!p[2]) {
            *s += 2;
            return '?';
        }
        if (!p[3]) {
            *s += 3;
            return '?';
        }
        c = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *s += 4;
        return c;
    }

    (*s)++;
    return '?';
}

static const font_glyph_t *find_glyph(const font_info_t *font, uint32_t codepoint)
{
    int low = 0;
    int high = font->glyph_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint32_t cp = font->glyphs[mid].codepoint;
        if (cp == codepoint) {
            return &font->glyphs[mid];
        } else if (cp < codepoint) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (codepoint != '?') {
        return find_glyph(font, '?');
    }
    return NULL;
}

static inline void unpack_rgb565(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint16_t c = (color >> 8) | (color << 8);
    *r = (c >> 8) & 0xF8;
    *r |= (*r >> 5);
    *g = (c >> 3) & 0xFC;
    *g |= (*g >> 6);
    *b = (c << 3) & 0xF8;
    *b |= (*b >> 5);
}

static inline uint16_t blend_pixels(uint16_t bg_color, uint16_t fg_color, uint8_t alpha)
{
    if (alpha == 0) return bg_color;
    if (alpha == 15) return fg_color;

    uint8_t bg_r, bg_g, bg_b;
    uint8_t fg_r, fg_g, fg_b;

    unpack_rgb565(bg_color, &bg_r, &bg_g, &bg_b);
    unpack_rgb565(fg_color, &fg_r, &fg_g, &fg_b);

    uint8_t blended_r = (fg_r * alpha + bg_r * (15 - alpha)) / 15;
    uint8_t blended_g = (fg_g * alpha + bg_g * (15 - alpha)) / 15;
    uint8_t blended_b = (fg_b * alpha + bg_b * (15 - alpha)) / 15;

    return rgb565(blended_r, blended_g, blended_b);
}

static void fb_draw_char_aa(int x, int y, const font_info_t *font, const font_glyph_t *glyph, uint16_t color)
{
    if (glyph->width == 0 || glyph->height == 0) return;

    uint32_t offset = glyph->bitmap_offset;

    for (int row = 0; row < glyph->height; row++) {
        int target_y = y + glyph->bearing_y + row;
        if (target_y < 0 || target_y >= LCD_V_RES) continue;

        for (int col = 0; col < glyph->width; col++) {
            int target_x = x + glyph->bearing_x + col;
            if (target_x < 0 || target_x >= LCD_H_RES) continue;

            uint32_t pixel_idx = row * glyph->width + col;
            uint32_t byte_idx = offset + (pixel_idx / 2);
            uint8_t byte_val = font->bitmaps[byte_idx];
            uint8_t alpha;
            if (pixel_idx % 2 == 0) {
                alpha = byte_val >> 4;
            } else {
                alpha = byte_val & 0x0F;
            }

            if (alpha > 0) {
                uint32_t fb_idx = target_y * LCD_H_RES + target_x;
                s_fb[fb_idx] = blend_pixels(s_fb[fb_idx], color, alpha);
            }
        }
    }
}

static int fb_draw_string_aa(int x, int y, const font_info_t *font, const char *str, uint16_t color)
{
    int cx = x;
    const char *p = str;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;
        const font_glyph_t *glyph = find_glyph(font, cp);
        if (glyph) {
            fb_draw_char_aa(cx, y, font, glyph, color);
            cx += glyph->advance;
        }
    }
    return cx - x;
}

static int str_width_aa(const font_info_t *font, const char *str)
{
    int width = 0;
    const char *p = str;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;
        const font_glyph_t *glyph = find_glyph(font, cp);
        if (glyph) {
            width += glyph->advance;
        }
    }
    return width;
}

esp_err_t bsp_display_init(void)
{
    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(LCD_PIN_BL, 1);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_fb = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ST7789 display initialised");
    return ESP_OK;
}

void bsp_display_set_wifi_connected(bool connected)
{
    s_wifi_connected = connected;
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= LCD_V_RES) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= LCD_H_RES) continue;
            s_fb[row * LCD_H_RES + col] = color;
        }
    }
}

static void fb_clear(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        s_fb[i] = color;
    }
}

void bsp_display_show_number(uint32_t value)
{
    if (!s_panel || !s_fb) return;

    const uint16_t bg = rgb565(0, 0, 0);
    const uint16_t fg = rgb565(0, 255, 120);

    fb_clear(bg);

    char buf[12];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);

    int w = str_width_aa(&roboto_title, buf);
    int x = (LCD_H_RES - w) / 2;
    int y = (LCD_V_RES - roboto_title.height) / 2 + roboto_title.ascender;
    fb_draw_string_aa(x, y, &roboto_title, buf, fg);

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
}

static uint16_t get_wifi_rssi_color(int rssi)
{
    if (rssi >= -60) {
        return rgb565(0, 255, 120);   // Excellent: green
    } else if (rssi >= -70) {
        return rgb565(100, 255, 100);  // Good: vibrant light green
    } else if (rssi >= -80) {
        return rgb565(255, 220, 0);   // Fair: yellow
    } else {
        return rgb565(255, 50, 50);   // Poor: red
    }
}

static void fb_draw_wifi_indicator(int x, int y)
{
    if (!s_wifi_connected) {
        return; // Not connected, don't draw indicator
    }

    int rssi = -128;
    if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK) {
        return; // Not connected, don't draw indicator
    }

    int active_bars = 0;
    if (rssi >= -60) active_bars = 4;
    else if (rssi >= -70) active_bars = 3;
    else if (rssi >= -80) active_bars = 2;
    else if (rssi >= -90) active_bars = 1;

    uint16_t inactive_col = rgb565(60, 60, 60);
    uint16_t active_col = get_wifi_rssi_color(rssi);

    // Draw 4 bars of increasing height
    for (int i = 0; i < 4; i++) {
        uint16_t col = (i < active_bars) ? active_col : inactive_col;
        int bar_h = (i + 1) * 4;
        fb_fill_rect(x + i * 5, y + 16 - bar_h, 3, bar_h, col);
    }

    // Draw Wi-Fi signal strength in dB (e.g. "-65 dB") to the left of the icon in white
    char rssi_str[16];
    snprintf(rssi_str, sizeof(rssi_str), "%d dB", rssi);
    int text_w = str_width_aa(&roboto_body, rssi_str);
    fb_draw_string_aa(x - text_w - 6, y, &roboto_body, rssi_str, rgb565(255, 255, 255));
}

void bsp_display_show_lines(const char *title, const char *const *lines, int n_lines)
{
    if (!s_panel || !s_fb) return;

    const uint16_t bg = rgb565(0, 0, 0);
    const uint16_t title_col = rgb565(0, 255, 120);
    const uint16_t text_col = rgb565(255, 255, 255);

    fb_clear(bg);

    // Draw Wi-Fi signal strength indicator at the top right
    fb_draw_wifi_indicator(218, 10);

    // Draw Wi-Fi channel indicator at the top left in white
    if (s_wifi_connected) {
        uint8_t primary_chan = 0;
        wifi_second_chan_t second_chan;
        if (esp_wifi_get_channel(&primary_chan, &second_chan) == ESP_OK) {
            char ch_str[16];
            snprintf(ch_str, sizeof(ch_str), "CH: %d", primary_chan);
            fb_draw_string_aa(6, 10, &roboto_body, ch_str, rgb565(255, 255, 255));
        }
    }

    int y = 48; // baseline for title

    if (title && title[0]) {
        int w = str_width_aa(&roboto_title, title);
        int x = (LCD_H_RES - w) / 2;
        if (x < 4) x = 4;
        fb_draw_string_aa(x, y, &roboto_title, title, title_col);
        y += 40; // advance baseline for body
    } else {
        y = 60;  // baseline if no title
    }

    int line_h = roboto_body.height + 6;
    if (line_h < 18) line_h = 18;

    for (int i = 0; i < n_lines; i++) {
        if (!lines[i]) continue;
        int w = str_width_aa(&roboto_body, lines[i]);
        int x = (LCD_H_RES - w) / 2;
        if (x < 4) x = 4;
        fb_draw_string_aa(x, y, &roboto_body, lines[i], text_col);
        y += line_h;
    }

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
}
