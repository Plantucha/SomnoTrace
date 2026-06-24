# Hardware reference — Waveshare ESP32-S3-Touch-LCD-1.54

Concise board reference for development. Read only when you need pin/peripheral
details. Source: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54>.

> Note: there is also a non-touch variant ("ESP32-S3-LCD-1.54"). This board is
> the **touch** version (CST816 controller present).

## Core specs

- **MCU:** ESP32-S3R8 (dual-core Xtensa LX7 @ 240 MHz), Wi-Fi 2.4 GHz + BLE 5.
- **Memory:** 8 MB PSRAM, 16 MB NOR flash.
- **Display:** 1.54" 240×240 LCD, **ST7789** controller, 4-wire SPI.
- **Touch:** **CST816** capacitive controller, I2C.
- **Audio:** **ES8311** codec + **ES7210** ADC (echo cancel) over I2S;
  **NS4150B** amplifier (enabled via PA CTRL).
- **IMU:** **QMI8658** 6-axis (I2C, INT line).
- **Other:** RTC, microSD (TF) slot (SDMMC 4-bit), USB Type-C (native
  USB-Serial-JTAG), Li-battery charge/discharge interface, chip antenna.

## Buttons / boot

- Physical buttons: **PWR** (IO5), **PLUS** (IO4), **BOOT** (IO0).
- **IO0 = BOOT strapping pin** (labelled "KEY MINUS" in the pin table). Hold it
  during reset/connect to force ROM **download mode** for flashing.
- No dedicated EN/RESET button is exposed.

## GPIO map (from board pinout table)

| GPIO | Function(s) |
|------|-------------|
| IO0  | KEY MINUS / **BOOT** strap |
| IO1  | BAT ADC (battery voltage sense) |
| IO2  | BAT EN (battery power latch/enable) |
| IO3  | CHG STAT (charger status) |
| IO4  | KEY PLUS |
| IO5  | KEY PWR |
| IO6  | IMU INT |
| IO7  | PA CTRL (audio amp enable) |
| IO8  | I2S MCLK |
| IO9  | I2S SCLK |
| IO10 | I2S LRCK |
| IO11 | I2S ASDOUT |
| IO12 | I2S DSDIN |
| IO13 | SD D2 |
| IO14 | SD D3 |
| IO15 | SD CMD |
| IO16 | SD CLK |
| IO17 | SD D0 |
| IO18 | SD D1 |
| IO19 | USB D- (USB_N) |
| IO20 | USB D+ (USB_P) |
| IO21 | LCD CS |
| IO38 | LCD CLK (SCLK) |
| IO39 | LCD DIN (MOSI) |
| IO40 | LCD RST |
| IO41 | I2C SCL (shared: touch + codec + IMU) |
| IO42 | I2C SDA (shared: touch + codec + IMU) |
| IO43 | ESP TXD (UART0 TX) |
| IO44 | ESP RXD (UART0 RX) |
| IO45 | LCD DC |
| IO46 | LCD BL (backlight) |
| IO47 | TP RST (touch reset) |
| IO48 | TP INT (touch interrupt) |

### Buses at a glance

- **LCD (SPI):** CS=IO21, CLK=IO38, DIN/MOSI=IO39, DC=IO45, RST=IO40, BL=IO46.
- **Shared I2C:** SCL=IO41, SDA=IO42 — CST816 touch, ES8311/ES7210 codec,
  QMI8658 IMU all sit on this bus. Touch also uses RST=IO47, INT=IO48.
- **I2S audio:** MCLK=IO8, SCLK=IO9, LRCK=IO10, ASDOUT=IO11, DSDIN=IO12,
  PA enable=IO7.
- **microSD (SDMMC 4-bit):** CLK=IO16, CMD=IO15, D0=IO17, D1=IO18, D2=IO13,
  D3=IO14.
- **Battery:** ADC=IO1, enable/latch=IO2, charger status=IO3.

## Further reading (fetch only if needed)

- Main docs: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54>
- ESP-IDF setup: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Development-Environment-Setup-ESPIDF>
- Resources (schematic PDF, datasheets): <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents>
- Code examples (Apache-2.0; reference only, do not copy — clean-room):
  <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54>
