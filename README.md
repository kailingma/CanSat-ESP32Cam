# CanSat-ESP32Cam

An ESP32-CAM imaging module for the U-Hill Physineering Club CanSat project (2026). Captures and transmits photos on demand for aerial survey applications.

## Features

- On-demand image capture via ESP32-CAM module
- Lightweight C++ firmware
- Integration-ready for CanSat platform
- Remote triggering support

## Getting Started

### Requirements
- ESP32-CAM module
- USB-to-UART programmer
- Arduino IDE or PlatformIO

### Setup

1. Clone this repository
2. Configure your ESP32-CAM board settings in the IDE
3. Upload the sketch to your module
4. Trigger image capture via serial commands or API endpoints

## Usage

Connect to the ESP32-CAM and send capture commands to retrieve images. Details on command formats and integration points are documented in the source code.

## Project Context

Part of the 2026 CanSat competition, this module handles aerial imaging for the U-Hill Physineering Club's high-altitude balloon project.

## License

Check repository for license details.