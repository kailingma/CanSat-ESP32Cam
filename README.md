# CanSat-ESP32Cam

ESP32-CAM firmware for the U-Hill Physineering Club CanSat project (2026). An Arduino pulls GPIO 12 LOW to trigger a photo capture; the ESP32-CAM saves the image to SD card (SPIFFS as fallback) and reports status back over UART.

## How it works

1. Arduino pulls **GPIO 12** LOW → interrupt fires on the ESP32-CAM
2. ESP32-CAM sends `READY` over Serial2
3. Arduino sends the target filepath (e.g. `/run01/00042.jpg`) over Serial2
4. ESP32-CAM captures the image and saves it; replies `OK` or an `ERR:…` message

## Pin Configuration

| Pin | Role |
|-----|------|
| GPIO 12 | Trigger input — Arduino pulls LOW to request capture |
| GPIO 14 | Serial2 RX — receives filepath from Arduino |
| GPIO 15 | Serial2 TX — sends status messages to Arduino |
| GPIO 13 | SD card SPI chip-select (onboard microSD) |
| GPIO 0  | Camera XCLK |
| GPIO 26 | Camera SIOD (I²C SDA) |
| GPIO 27 | Camera SIOC (I²C SCL) |
| GPIO 25 | Camera VSYNC |
| GPIO 23 | Camera HREF |
| GPIO 22 | Camera PCLK |
| GPIO 35/34/39/36/21/19/18/5 | Camera data bus (Y9–Y2) |

Camera pins follow the standard AI-Thinker ESP32-CAM layout.

## Serial / UART

| Interface | Baud | Purpose |
|-----------|------|---------|
| Serial (USB) | 115200 | Debug output / serial monitor terminal |
| Serial2 (GPIO 14/15) | 9600, 8N1 | Communication with Arduino |

## Main Functions

| Function | Description |
|----------|-------------|
| `captureAndSave(filepath)` | Captures a JPEG from the camera and writes it to the given path on SD card or SPIFFS |
| `listFiles(path)` | Prints all files and directories under `path` to Serial |
| `deleteFile(path)` | Deletes a single file from storage |
| `formatStorage()` | Wipes all storage (asks for confirmation first) |
| `simulateCapture()` | Triggers a capture without an Arduino — saves to `/fail/XXXX.jpg` |
| `startWebServer()` | Starts a WiFi AP (`ESP32-CAM-Browser`) and HTTP server on port 80 with a file browser and live snapshot endpoint |
| `stopWebServer()` | Shuts down the HTTP server and WiFi AP |

## Storage

SD card is used when available. If the SD card is missing or fails to mount, the firmware falls back to SPIFFS (internal flash). Failed captures (no filepath received within 5 seconds) are saved to `/fail/` with an auto-incrementing counter.

## Terminal Commands

Type commands into the Serial monitor (115200 baud):

| Command | Effect |
|---------|--------|
| `ls` | List all files |
| `ls <path>` | List files under a specific path |
| `del <path>` | Delete a file |
| `fmt` | Format storage (prompts for confirmation) |
| `capture` | Simulate a trigger and capture one photo |
| `start` | Start WiFi AP and web file browser |
| `stop` | Stop web file browser and WiFi AP |
| `help` | Show command list |

## Deployment

1. Open `cansat-cam.ino` in Arduino IDE with the **AI-Thinker ESP32-CAM** board selected
2. Upload the sketch via a USB-to-UART programmer
3. Open Serial monitor at **115200 baud** to see debug output
4. Insert an SD card before powering on (or SPIFFS will be used automatically)
