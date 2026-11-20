#pragma once

// ============================================================
// config.h — ESP32-CAM AI Thinker
// ============================================================
// Edit values here. Do NOT commit Wi-Fi credentials to VCS.
// ============================================================

// ------------------------------------------------------------
// Wi-Fi Credentials
// ------------------------------------------------------------
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define WIFI_CONNECT_TIMEOUT_MS    20000
#define WIFI_RECONNECT_INTERVAL_MS  5000

// ------------------------------------------------------------
// HTTP Server
// ------------------------------------------------------------
#define HTTP_SERVER_PORT 80

// ------------------------------------------------------------
// Camera — AI Thinker Pin Mapping
// Source: framework-arduinoespressif32 camera_pins.h
// ------------------------------------------------------------
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

#define CAM_FLASH_LED_GPIO  4
#define CAM_STATUS_LED_GPIO 33

// ------------------------------------------------------------
// Camera — Base Configuration
// ------------------------------------------------------------
// XCLK: 20 MHz is the standard value for OV2640/OV5640.
#define CAM_XCLK_FREQ_HZ  20000000

// JPEG quality: 0 = best (largest file), 63 = worst (smallest).
// 10 gives good detail for plant/insect inspection.
#define CAM_JPEG_QUALITY  10

// Frame buffer count.
// 1 = safest (no overflow). 2 = double-buffer for smoother stream.
// Without PSRAM always use 1 — DRAM is too small for 2 large frames.
#define CAM_FB_COUNT_NO_PSRAM    1
#define CAM_FB_COUNT_WITH_PSRAM  2

// Default frame size on startup.
// QVGA (320×240) is the safest starting point; change via /set_resolution.
// Without PSRAM: cap at SVGA (800×600) to stay within DRAM limits.
#define CAM_FRAMESIZE_NO_PSRAM   FRAMESIZE_SVGA
#define CAM_FRAMESIZE_WITH_PSRAM FRAMESIZE_QVGA   // startup default

// ------------------------------------------------------------
// Camera — Image Quality Defaults
// These are applied once after esp_camera_init() and can be
// changed at runtime via GET /set_quality.
//
// Brightness  : -2 to +2  (0 = normal)
// Contrast    : -2 to +2  (0 = normal)
// Saturation  : -2 to +2  (0 = normal)
// AE level    : -2 to +2  (0 = auto)
// AEC value   : 0–1200    (manual shutter; 0 = use auto AEC)
// AWB gain    : 0 or 1    (1 = enable auto white balance gain)
// White bal   : 0 or 1    (1 = enable AWB)
// Gain ceiling: GAINCEILING_2X … GAINCEILING_128X
// ------------------------------------------------------------
#define CAM_DEFAULT_BRIGHTNESS   0
#define CAM_DEFAULT_CONTRAST     0
#define CAM_DEFAULT_SATURATION   0
#define CAM_DEFAULT_AE_LEVEL     0
#define CAM_DEFAULT_AWB_GAIN     1
#define CAM_DEFAULT_WHITEBAL     1
#define CAM_DEFAULT_AEC2         1    // enhanced auto-exposure
#define CAM_DEFAULT_GAINCEILING  GAINCEILING_8X

// ------------------------------------------------------------
// Serial Monitor
// ------------------------------------------------------------
#define SERIAL_BAUD_RATE 115200

// ------------------------------------------------------------
// Debug / Test Flags
// ------------------------------------------------------------
// Set to 1 to boot without Wi-Fi (camera-only test mode).
// Set to 0 for normal operation with HTTP server.
#define WIFI_SKIP_FOR_CAM_TEST 0
