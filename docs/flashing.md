# Web Flashing Guide

> Flash SomnoTrace onto your device directly from your web browser — no coding, command line, or software installation required!

---

## What You Need

1. **Waveshare ESP32-S3-Touch-LCD-1.54** board (Touch variant with battery preferred — [Buy on Waveshare](https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33869)).
2. **USB-C Data Cable** (make sure it is a *data* cable, not a charge-only cable).
3. **Computer with a modern browser**: Google Chrome, Microsoft Edge, Brave, or Opera (these browsers support Web Serial).
4. **MicroSD card** (formatted as FAT32 or exFAT, 8 GB to 32 GB recommended) inserted into the board's SD slot.

---

## Step-by-Step Instructions

### Step 1: Open the Web Flasher

1. Connect your Waveshare board to your computer using a **USB-C data cable**.
2. Open **[somnotrace.com](https://somnotrace.com)** in a Web Serial browser (Chrome, Edge, Brave, or Opera).

---

### Step 2: Install in 3 Clicks

1. Click **Install SomnoTrace**.
2. In the browser popup, select **`USB JTAG/serial debug unit`** (or your device's COM / tty port).
3. Click **Connect**.

That's it! SomnoTrace handles everything automatically:
- Fetches the latest stable release directly from GitHub.
- Puts the ESP32-S3 into ROM download mode programmatically.
- Flashes the complete firmware image.
- Automatically reboots the device into SomnoTrace when complete.

> **Manual Boot Fallback:** If your device doesn't enter download mode automatically within 7 seconds, unplug the USB-C cable, press and **hold the BOOT button** (the leftmost physical button on the top edge), plug the USB-C cable back in, then release the button and click Install.

---

### Step 3: First Boot & Wi-Fi Setup

1. The LCD display will illuminate with the **SomnoTrace** logo and indicate setup mode.
2. On your phone or computer, connect to the Wi-Fi network named **`SomnoTrace-Setup`**.
3. A captive portal setup page opens automatically (or visit `http://somnotrace.local` / `http://192.168.4.1`):
   - Select your home Wi-Fi network and enter your password.
   - Click **Save & Connect**.
4. SomnoTrace connects to your home network and displays its IP address and clock on the LCD screen.

You can now open any browser on your local network and visit:
```text
http://somnotrace.local
```

---

### Step 4: Pair Your AirSense 11

Once Wi-Fi is configured, pair SomnoTrace with your CPAP over Bluetooth:

👉 **[AirSense 11 Pairing Guide](pairing.md)**

---

## Troubleshooting

- **Browser doesn't show the Connect popup?**  
  Make sure you are using Google Chrome or Microsoft Edge. Apple Safari and Mozilla Firefox do not support Web Serial.
- **No COM / Serial device appears in the list?**  
  - Try another USB-C cable (many cables included with phones/vapes are power-only and cannot transfer data).
  - Hold the **BOOT** button while plugging the board into your computer.
- **SD Card Error on screen?**  
  Ensure your MicroSD card is inserted securely into the slot before powering on, and that it is formatted as FAT32 or exFAT.
