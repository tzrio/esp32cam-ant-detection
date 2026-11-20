// ============================================================
// main.cpp — ESP32-CAM AI Thinker
// ============================================================
// Endpoints:
//   GET /               — status + control webpage
//   GET /capture        — single JPEG frame
//   GET /stream         — MJPEG live stream (FreeRTOS task)
//   GET /set_resolution — change frame size at runtime
//   GET /set_quality    — change JPEG + image quality at runtime
//   GET /flash          — turn flash LED on or off (?state=on|off)
//   GET /status         — JSON system status
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "config.h"

// ============================================================
// Resolution table — OV2640 verified sizes
// needsPsram: frame buffer must live in PSRAM (>= VGA for safety)
// captureOnly: too slow/large for smooth streaming
// ============================================================
struct ResEntry {
    const char *name;
    framesize_t size;
    uint16_t    width;
    uint16_t    height;
    bool        needsPsram;
    bool        captureOnly;  // true = not suitable for MJPEG stream
};

static const ResEntry RESOLUTIONS[] = {
    { "QVGA", FRAMESIZE_QVGA, 320,  240,  false, false },
    { "VGA",  FRAMESIZE_VGA,  640,  480,  false, false },
    { "SVGA", FRAMESIZE_SVGA, 800,  600,  true,  false },
    { "HD",   FRAMESIZE_HD,   1280, 720,  true,  true  },
    { "FHD",  FRAMESIZE_FHD,  1920, 1080, true,  true  },
};
static const int NUM_RESOLUTIONS = sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0]);

// ---- Forward declarations ----------------------------------
static bool   initCamera();
static bool   connectWiFi();
static void   startWebServer();
static void   applySensorDefaults();

static void   handleRoot();
static void   handleCapture();
static void   handleStream();
static void   handleSetResolution();
static void   handleSetCaptureRes();
static void   handleSetQuality();
static void   handleFlash();
static void   handleStatus();
static void   handleNotFound();

static String buildStatusPage();
static String resolutionName(framesize_t fs);
static const ResEntry *findResolution(const String &name);

// ---- Globals -----------------------------------------------
static WebServer    server(HTTP_SERVER_PORT);
static bool         cameraReady      = false;
static bool         wifiReady        = false;
static bool         streamActive     = false;
static bool         flashOn          = false;
// Stream resolution — kept low for smooth MJPEG (default QVGA)
static framesize_t  streamFrameSize  = CAM_FRAMESIZE_WITH_PSRAM;
// Capture resolution — can be much higher, applied only during /capture
static framesize_t  captureFrameSize = FRAMESIZE_FHD;
static int          currentQuality   = CAM_JPEG_QUALITY;
static TaskHandle_t streamTaskHandle = NULL;

// ---- FreeRTOS stream task ----------------------------------
struct StreamTaskParams { WiFiClient client; };

static void streamTask(void *pvParameters)
{
    StreamTaskParams *p = static_cast<StreamTaskParams *>(pvParameters);
    WiFiClient client   = p->client;
    delete p;

    Serial.println("[STREAM] Task started");

    while (client.connected()) {
        // Skip frame if capture is in progress on the other core
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            delay(100);
            continue;
        }

        String part =
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: " + String(fb->len) + "\r\n"
            "\r\n";
        client.print(part);
        client.write(fb->buf, fb->len);
        client.print("\r\n");

        esp_camera_fb_return(fb);
        delay(50);  // ~20 fps cap; reduces EV-EOF-OVF at QVGA
    }

    Serial.println("[STREAM] Client disconnected");
    streamActive     = false;
    streamTaskHandle = NULL;
    vTaskDelete(NULL);
}

// ============================================================
// setup()
// ============================================================
void setup()
{
    Serial.begin(SERIAL_BAUD_RATE);
    delay(2000);

    setCpuFrequencyMhz(160);   // cooler, still plenty for camera+Wi-Fi
    btStop();                  // Bluetooth unused — save power

    // Flash LED on GPIO 4 — configure as output, start OFF.
    // GPIO 4 is shared with the camera data bus (Y2) during init,
    // so we set it up BEFORE esp_camera_init() takes over.
    pinMode(CAM_FLASH_LED_GPIO, OUTPUT);
    digitalWrite(CAM_FLASH_LED_GPIO, LOW);
    flashOn = false;

    Serial.println();
    Serial.println("============================================");
    Serial.println("  ESP32-CAM AI Thinker — Image Quality Test");
    Serial.println("============================================");
    Serial.printf("[SYS] CPU: %u MHz\n", getCpuFrequencyMhz());

    if (psramFound()) {
        Serial.printf("[SYS] PSRAM: %u bytes free\n", ESP.getFreePsram());
    } else {
        Serial.println("[SYS] PSRAM: not detected");
    }
    Serial.printf("[SYS] Heap before init: %u bytes\n", ESP.getFreeHeap());

    cameraReady = initCamera();

#if WIFI_SKIP_FOR_CAM_TEST
    Serial.println("[NET] Wi-Fi skipped (WIFI_SKIP_FOR_CAM_TEST=1)");
#else
    wifiReady = connectWiFi();
    if (wifiReady) startWebServer();
    else Serial.println("[NET] Wi-Fi failed — HTTP server not started");
#endif

    Serial.println("[SYS] Setup complete");
    Serial.printf("[SYS] Heap after init: %u bytes\n", ESP.getFreeHeap());
}

// ============================================================
// loop()
// ============================================================
void loop()
{
    if (wifiReady) server.handleClient();

    static unsigned long lastReconnect = 0;
    if (wifiReady && WiFi.status() != WL_CONNECTED) {
        unsigned long now = millis();
        if (now - lastReconnect >= WIFI_RECONNECT_INTERVAL_MS) {
            lastReconnect = now;
            Serial.println("[NET] Wi-Fi lost — reconnecting...");
            WiFi.reconnect();
        }
    }
}

// ============================================================
// initCamera()
// ============================================================
static bool initCamera()
{
    Serial.println("[CAM] Initialising camera...");

    camera_config_t cfg;
    cfg.pin_pwdn     = CAM_PIN_PWDN;
    cfg.pin_reset    = CAM_PIN_RESET;
    cfg.pin_xclk     = CAM_PIN_XCLK;
    cfg.pin_sccb_sda = CAM_PIN_SIOD;
    cfg.pin_sccb_scl = CAM_PIN_SIOC;
    cfg.pin_d7       = CAM_PIN_D7;
    cfg.pin_d6       = CAM_PIN_D6;
    cfg.pin_d5       = CAM_PIN_D5;
    cfg.pin_d4       = CAM_PIN_D4;
    cfg.pin_d3       = CAM_PIN_D3;
    cfg.pin_d2       = CAM_PIN_D2;
    cfg.pin_d1       = CAM_PIN_D1;
    cfg.pin_d0       = CAM_PIN_D0;
    cfg.pin_vsync    = CAM_PIN_VSYNC;
    cfg.pin_href     = CAM_PIN_HREF;
    cfg.pin_pclk     = CAM_PIN_PCLK;
    cfg.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        streamFrameSize      = CAM_FRAMESIZE_WITH_PSRAM;
        captureFrameSize     = FRAMESIZE_FHD;
        cfg.frame_size       = streamFrameSize;
        cfg.jpeg_quality     = CAM_JPEG_QUALITY;
        cfg.fb_count         = CAM_FB_COUNT_WITH_PSRAM;
        cfg.fb_location      = CAMERA_FB_IN_PSRAM;
        cfg.grab_mode        = CAMERA_GRAB_LATEST;
        Serial.println("[CAM] PSRAM mode: stream=QVGA, capture=FHD");
    } else {
        streamFrameSize      = CAM_FRAMESIZE_NO_PSRAM;
        captureFrameSize     = CAM_FRAMESIZE_NO_PSRAM;
        cfg.frame_size       = streamFrameSize;
        cfg.jpeg_quality     = CAM_JPEG_QUALITY;
        cfg.fb_count         = CAM_FB_COUNT_NO_PSRAM;
        cfg.fb_location      = CAMERA_FB_IN_DRAM;
        cfg.grab_mode        = CAMERA_GRAB_WHEN_EMPTY;
        Serial.println("[CAM] No PSRAM: stream=SVGA, capture=SVGA");
    }

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init failed: 0x%x\n", err);
        Serial.println("[CAM] Check ribbon cable and 5V supply");
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        Serial.printf("[CAM] Sensor PID: 0x%04X\n", s->id.PID);
        applySensorDefaults();
    }

    Serial.printf("[CAM] Init OK — stream: %s  capture: %s\n",
                  resolutionName(streamFrameSize).c_str(),
                  resolutionName(captureFrameSize).c_str());
    return true;
}

// ============================================================
// applySensorDefaults()
// Called once after init and after resolution changes.
// ============================================================
static void applySensorDefaults()
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;

    s->set_brightness(s,  CAM_DEFAULT_BRIGHTNESS);
    s->set_contrast(s,    CAM_DEFAULT_CONTRAST);
    s->set_saturation(s,  CAM_DEFAULT_SATURATION);
    s->set_ae_level(s,    CAM_DEFAULT_AE_LEVEL);
    s->set_awb_gain(s,    CAM_DEFAULT_AWB_GAIN);
    s->set_whitebal(s,    CAM_DEFAULT_WHITEBAL);
    s->set_aec2(s,        CAM_DEFAULT_AEC2);
    s->set_gainceiling(s, CAM_DEFAULT_GAINCEILING);
}

// ============================================================
// connectWiFi()
// ============================================================
static bool connectWiFi()
{
    Serial.printf("[NET] Connecting to: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 >= WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("\n[NET] Timeout — check SSID/password in config.h");
            return false;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("[NET] Connected — IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

// ============================================================
// startWebServer()
// ============================================================
static void startWebServer()
{
    server.on("/",                HTTP_GET, handleRoot);
    server.on("/capture",         HTTP_GET, handleCapture);
    server.on("/stream",          HTTP_GET, handleStream);
    server.on("/set_resolution",  HTTP_GET, handleSetResolution);
    server.on("/set_capture_res", HTTP_GET, handleSetCaptureRes);
    server.on("/set_quality",     HTTP_GET, handleSetQuality);
    server.on("/flash",           HTTP_GET, handleFlash);
    server.on("/status",          HTTP_GET, handleStatus);
    server.onNotFound(handleNotFound);
    server.begin();

    String ip = WiFi.localIP().toString();
    Serial.printf("[HTTP] Listening on port %d\n", HTTP_SERVER_PORT);
    Serial.printf("[HTTP] /               http://%s/\n",               ip.c_str());
    Serial.printf("[HTTP] /capture        http://%s/capture\n",        ip.c_str());
    Serial.printf("[HTTP] /stream         http://%s/stream\n",         ip.c_str());
    Serial.printf("[HTTP] /flash          http://%s/flash?state=on\n", ip.c_str());
    Serial.printf("[HTTP] /set_resolution http://%s/set_resolution?res=VGA\n", ip.c_str());
    Serial.printf("[HTTP] /set_quality    http://%s/set_quality?quality=10&brightness=0&contrast=0&saturation=0\n", ip.c_str());
    Serial.printf("[HTTP] /status         http://%s/status\n",         ip.c_str());
}

// ============================================================
// handleRoot() — GET /
// ============================================================
static void handleRoot()
{
    server.send(200, "text/html", buildStatusPage());
}

// ============================================================
// handleCapture() — GET /capture
//
// High-resolution capture strategy:
//   1. If captureFrameSize != streamFrameSize, temporarily switch
//      the sensor to captureFrameSize.
//   2. Discard the first frame — it carries the old resolution's
//      pixel data while the sensor pipeline is re-locking.
//   3. Grab the second frame — this is the full-res image.
//   4. Restore the sensor to streamFrameSize so the next /stream
//      request works correctly.
//
// The whole sequence takes ~200–400 ms at FHD. That is acceptable
// for a still capture; it would be too slow for a live stream.
// ============================================================
static void handleCapture()
{
    if (!cameraReady) {
        server.send(503, "text/plain", "Camera not initialised");
        return;
    }
    if (streamActive) {
        server.send(503, "text/plain",
                    "Stop the stream before capturing");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    bool      needSwitch = (s != nullptr) &&
                           (captureFrameSize != streamFrameSize);

    // -- Step 1: switch to capture resolution ----------------
    if (needSwitch) {
        s->set_framesize(s, captureFrameSize);
        delay(200);  // sensor pipeline settle time
        // Discard first frame — still encoded at previous size
        camera_fb_t *discard = esp_camera_fb_get();
        if (discard) esp_camera_fb_return(discard);
        delay(50);
    }

    // -- Step 2: grab the real frame -------------------------
    unsigned long t0 = millis();
    camera_fb_t  *fb = esp_camera_fb_get();

    // -- Step 3: restore stream resolution immediately -------
    if (needSwitch && s) {
        s->set_framesize(s, streamFrameSize);
        delay(100);
    }

    if (!fb) {
        Serial.println("[CAM] fb_get() failed");
        server.send(503, "text/plain", "Frame capture failed");
        return;
    }

    unsigned long captureMs = millis() - t0;

    server.sendHeader("X-Image-Width",  String(fb->width));
    server.sendHeader("X-Image-Height", String(fb->height));
    server.sendHeader("X-Capture-Ms",   String(captureMs));
    server.sendHeader("X-JPEG-Size",    String(fb->len));
    server.sendHeader("X-Resolution",   resolutionName(captureFrameSize));
    server.sendHeader("Content-Disposition",
                      "inline; filename=capture_" +
                      resolutionName(captureFrameSize) + ".jpg");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");

    server.send_P(200, "image/jpeg",
                  reinterpret_cast<const char *>(fb->buf),
                  fb->len);

    Serial.printf("[CAM] Captured %s %ux%u  %u B  %lu ms\n",
                  resolutionName(captureFrameSize).c_str(),
                  fb->width, fb->height, fb->len, captureMs);

    esp_camera_fb_return(fb);
}

// ============================================================
// handleSetCaptureRes() — GET /set_capture_res?res=FHD
// Sets the resolution used only for still captures.
// Does not affect the stream resolution.
// ============================================================
static void handleSetCaptureRes()
{
    if (!cameraReady) {
        server.send(503, "text/plain", "Camera not initialised");
        return;
    }
    if (streamActive) {
        server.send(409, "text/plain",
                    "Stop stream before changing capture resolution");
        return;
    }

    String res = server.arg("res");
    if (res.isEmpty()) {
        String msg = "Capture resolution: " + resolutionName(captureFrameSize) + "\n";
        msg += "Stream  resolution: " + resolutionName(streamFrameSize) + "\n";
        msg += "Available for capture: QVGA, VGA, SVGA, HD, FHD";
        server.send(200, "text/plain", msg);
        return;
    }
    res.toUpperCase();

    const ResEntry *entry = findResolution(res);
    if (!entry) {
        server.send(400, "text/plain",
                    "Unknown resolution. Use: QVGA, VGA, SVGA, HD, FHD");
        return;
    }
    if (entry->needsPsram && !psramFound()) {
        server.send(400, "text/plain",
                    String(entry->name) + " requires PSRAM");
        return;
    }

    captureFrameSize = entry->size;
    String reply = "Capture resolution set to " + String(entry->name) +
                   " (" + String(entry->width) + "x" + String(entry->height) + ")";
    server.send(200, "text/plain", reply);
    Serial.printf("[CAM] Capture resolution → %s\n", entry->name);
}

// ============================================================
// handleStream() — GET /stream
// Spawns a FreeRTOS task on core 1; main loop stays free.
// ============================================================
static void handleStream()
{
    if (!cameraReady) {
        server.send(503, "text/plain", "Camera not initialised");
        return;
    }
    if (streamActive) {
        server.send(503, "text/plain", "Stream already active");
        return;
    }

    WiFiClient client = server.client();
    String header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    client.print(header);

    StreamTaskParams *params = new StreamTaskParams{client};
    streamActive = true;

    xTaskCreatePinnedToCore(
        streamTask, "stream_task",
        8192, params, 1,
        &streamTaskHandle, 1   // core 1; core 0 handles Wi-Fi + loop()
    );
    Serial.println("[STREAM] Task spawned on core 1");
}

// ============================================================
// handleFlash() — GET /flash?state=on|off|toggle
// Controls the onboard high-intensity white flash LED (GPIO 4).
// WARNING: the flash LED is very bright — do not stare at it.
// ============================================================
static void handleFlash()
{
    String state = server.arg("state");
    state.toLowerCase();

    if (state == "on") {
        digitalWrite(CAM_FLASH_LED_GPIO, HIGH);
        flashOn = true;
    } else if (state == "off") {
        digitalWrite(CAM_FLASH_LED_GPIO, LOW);
        flashOn = false;
    } else if (state == "toggle") {
        flashOn = !flashOn;
        digitalWrite(CAM_FLASH_LED_GPIO, flashOn ? HIGH : LOW);
    } else {
        // No argument — return current state
        server.send(200, "text/plain",
                    String("Flash is ") + (flashOn ? "ON" : "OFF") +
                    "\nUsage: /flash?state=on|off|toggle");
        return;
    }

    Serial.printf("[LED] Flash %s\n", flashOn ? "ON" : "OFF");
    server.send(200, "text/plain",
                String("Flash ") + (flashOn ? "ON" : "OFF"));
}

// ============================================================
// handleSetResolution() — GET /set_resolution?res=QVGA
// Sets the STREAM resolution only. Capture resolution is
// controlled separately via /set_capture_res.
// Only non-captureOnly resolutions are accepted here.
// ============================================================
static void handleSetResolution()
{
    if (!cameraReady) {
        server.send(503, "text/plain", "Camera not initialised");
        return;
    }
    if (streamActive) {
        server.send(409, "text/plain",
                    "Stop stream before changing resolution");
        return;
    }

    String res = server.arg("res");
    if (res.isEmpty()) {
        String msg = "Stream resolution: " + resolutionName(streamFrameSize) + "\n";
        msg += "Available for stream: QVGA, VGA, SVGA";
        server.send(200, "text/plain", msg);
        return;
    }
    res.toUpperCase();

    const ResEntry *entry = findResolution(res);
    if (!entry) {
        server.send(400, "text/plain",
                    "Unknown resolution. Use: QVGA, VGA, SVGA");
        return;
    }
    if (entry->captureOnly) {
        server.send(400, "text/plain",
                    String(entry->name) +
                    " is capture-only. Use /set_capture_res for still photos.");
        return;
    }
    if (entry->needsPsram && !psramFound()) {
        server.send(400, "text/plain",
                    String(entry->name) + " requires PSRAM");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        server.send(503, "text/plain", "Cannot access sensor");
        return;
    }

    int rc = s->set_framesize(s, entry->size);
    if (rc != 0) {
        server.send(500, "text/plain",
                    "set_framesize() failed for " + String(entry->name));
        return;
    }

    delay(300);
    streamFrameSize = entry->size;

    String reply = "Stream resolution set to " + String(entry->name) +
                   " (" + String(entry->width) + "x" + String(entry->height) + ")";
    server.send(200, "text/plain", reply);
    Serial.printf("[CAM] Stream resolution → %s\n", entry->name);
}

// ============================================================
// handleSetQuality() — GET /set_quality
//
// Query parameters (all optional — omit to keep current value):
//   quality    0–63  (JPEG compression; 0=best, 63=worst)
//   brightness -2–+2
//   contrast   -2–+2
//   saturation -2–+2
//   ae_level   -2–+2  (auto-exposure compensation)
//   aec2       0 or 1 (enhanced AEC on/off)
//   awb        0 or 1 (auto white balance on/off)
// ============================================================
static void handleSetQuality()
{
    if (!cameraReady) {
        server.send(503, "text/plain", "Camera not initialised");
        return;
    }
    if (streamActive) {
        server.send(409, "text/plain",
                    "Stop stream before changing quality settings");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        server.send(503, "text/plain", "Cannot access sensor");
        return;
    }

    String report = "";
    bool changed  = false;

    auto applyInt = [&](const char *arg,
                        int minVal, int maxVal,
                        const char *label,
                        int (*fn)(sensor_t *, int)) {
        if (server.hasArg(arg)) {
            int v = server.arg(arg).toInt();
            if (v < minVal || v > maxVal) {
                report += String(arg) + " out of range [" +
                          minVal + "," + maxVal + "]\n";
                return;
            }
            fn(s, v);
            report  += String(label) + " = " + v + "\n";
            changed  = true;
        }
    };

    if (server.hasArg("quality")) {
        int q = server.arg("quality").toInt();
        if (q < 0 || q > 63) {
            server.send(400, "text/plain", "quality must be 0–63");
            return;
        }
        s->set_quality(s, q);
        currentQuality = q;
        report  += "quality = " + String(q) + "\n";
        changed  = true;
    }

    applyInt("brightness", -2, 2, "brightness", s->set_brightness);
    applyInt("contrast",   -2, 2, "contrast",   s->set_contrast);
    applyInt("saturation", -2, 2, "saturation",  s->set_saturation);
    applyInt("ae_level",   -2, 2, "ae_level",   s->set_ae_level);
    applyInt("aec2",        0, 1, "aec2",        s->set_aec2);
    applyInt("awb",         0, 1, "awb",         s->set_whitebal);

    if (!changed) {
        String usage =
            "No parameters supplied.\n"
            "Usage: /set_quality?quality=10&brightness=0&contrast=0"
            "&saturation=0&ae_level=0&aec2=1&awb=1\n"
            "quality: 0 (best) to 63 (worst)\n"
            "brightness, contrast, saturation, ae_level: -2 to +2\n"
            "aec2, awb: 0 or 1\n";
        server.send(200, "text/plain", usage);
        return;
    }

    Serial.print("[CAM] Quality params updated: ");
    Serial.println(report);
    server.send(200, "text/plain", "Updated:\n" + report);
}

// ============================================================
// handleStatus() — GET /status
// Returns a JSON object for the Python capture script.
// ============================================================
static void handleStatus()
{
    String res   = resolutionName(streamFrameSize);
    String capRes= resolutionName(captureFrameSize);
    String ip    = wifiReady ? WiFi.localIP().toString() : "N/A";
    String rssi  = wifiReady ? String(WiFi.RSSI()) : "0";

    String json =
        "{\n"
        "  \"camera_ready\": "      + String(cameraReady  ? "true" : "false") + ",\n"
        "  \"stream_active\": "     + String(streamActive  ? "true" : "false") + ",\n"
        "  \"flash_on\": "          + String(flashOn        ? "true" : "false") + ",\n"
        "  \"wifi_connected\": "    + String(wifiReady     ? "true" : "false") + ",\n"
        "  \"ip\": \""              + ip                                        + "\",\n"
        "  \"rssi_dbm\": "          + rssi                                      + ",\n"
        "  \"psram\": "             + String(psramFound()  ? "true" : "false") + ",\n"
        "  \"stream_resolution\": \"" + res                                    + "\",\n"
        "  \"capture_resolution\": \"" + capRes                                + "\",\n"
        "  \"jpeg_quality\": "      + String(currentQuality)                   + ",\n"
        "  \"cpu_mhz\": "           + String(getCpuFrequencyMhz())              + ",\n"
        "  \"free_heap\": "         + String(ESP.getFreeHeap())                 + ",\n"
        "  \"free_psram\": "        + String(psramFound() ? ESP.getFreePsram() : 0) + "\n"
        "}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

// ============================================================
// handleNotFound() — 404
// ============================================================
static void handleNotFound()
{
    server.send(404, "text/plain",
                "Not found.\n"
                "Endpoints: /, /capture, /stream, "
                "/set_resolution, /set_quality, /status");
}

// ============================================================
// resolutionName() — framesize_t → human-readable string
// ============================================================
static String resolutionName(framesize_t fs)
{
    for (int i = 0; i < NUM_RESOLUTIONS; i++) {
        if (RESOLUTIONS[i].size == fs) return RESOLUTIONS[i].name;
    }
    return "UNKNOWN";
}

// ============================================================
// findResolution() — name string → ResEntry pointer
// Returns nullptr if not found.
// ============================================================
static const ResEntry *findResolution(const String &name)
{
    for (int i = 0; i < NUM_RESOLUTIONS; i++) {
        if (String(RESOLUTIONS[i].name) == name) return &RESOLUTIONS[i];
    }
    return nullptr;
}

// ============================================================
// buildStatusPage() — HTML for GET /
// ============================================================
static String buildStatusPage()
{
    String ip      = wifiReady   ? WiFi.localIP().toString()    : "N/A";
    String rssi    = wifiReady   ? String(WiFi.RSSI()) + " dBm" : "N/A";
    String sResName = resolutionName(streamFrameSize);
    String cResName = resolutionName(captureFrameSize);
    bool   hasPsram = psramFound();

    // Dot colours for status indicators
    auto dot = [](bool ok) -> String {
        return ok
            ? "<span style='color:#34d399'>&#9679;</span>"
            : "<span style='color:#f87171'>&#9679;</span>";
    };

    String html = R"(<!DOCTYPE html>
<html lang='en'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP32-CAM</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
     background:#0f0f0f;color:#e5e5e5;min-height:100vh;padding:16px}
header{padding:20px 0 12px;border-bottom:1px solid #2a2a2a;margin-bottom:20px}
header h1{font-size:1.3em;font-weight:600;letter-spacing:.02em}
header p{font-size:.8em;color:#666;margin-top:3px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
@media(max-width:480px){.grid{grid-template-columns:1fr}}
section{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:10px;padding:16px}
section h2{font-size:.75em;font-weight:600;letter-spacing:.08em;
           text-transform:uppercase;color:#666;margin-bottom:12px}
.stat{display:flex;justify-content:space-between;align-items:center;
      padding:6px 0;border-bottom:1px solid #222;font-size:.85em}
.stat:last-child{border-bottom:none}
.stat .k{color:#888}
.stat .v{font-weight:500}
.green{color:#34d399}.red{color:#f87171}.amber{color:#fbbf24}
.pill{display:inline-block;padding:2px 8px;border-radius:20px;
      font-size:.75em;font-weight:600}
.pill-green{background:#052e16;color:#34d399}
.pill-red{background:#2d0a0a;color:#f87171}
.pill-amber{background:#2d1a00;color:#fbbf24}
.pill-grey{background:#222;color:#888}
.btn-row{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
button{padding:8px 16px;border:none;border-radius:7px;font-size:.85em;
       font-weight:600;cursor:pointer;transition:opacity .15s}
button:active{opacity:.7}
.btn-primary{background:#3b82f6;color:#fff}
.btn-success{background:#059669;color:#fff}
.btn-danger{background:#dc2626;color:#fff}
.btn-neutral{background:#2a2a2a;color:#ccc}
.btn-amber{background:#d97706;color:#fff}
select,input[type=range],input[type=number]{
  width:100%;background:#111;color:#e5e5e5;
  border:1px solid #333;border-radius:6px;padding:6px 8px;
  font-size:.85em;margin-top:6px;outline:none}
select:focus,input:focus{border-color:#3b82f6}
label{display:block;font-size:.8em;color:#888;margin-top:12px}
label:first-of-type{margin-top:0}
.range-row{display:flex;align-items:center;gap:8px}
.range-row input{flex:1}
.range-val{min-width:24px;text-align:right;font-size:.8em;color:#aaa}
.feedback{font-size:.8em;color:#34d399;margin-top:8px;min-height:16px}
#stream-wrap{position:relative;background:#111;border-radius:8px;
             overflow:hidden;margin-top:12px;min-height:60px}
#stream-wrap img{width:100%;display:block;border-radius:8px}
#stream-badge{position:absolute;top:8px;right:8px;padding:3px 10px;
              border-radius:20px;font-size:.72em;font-weight:700;
              background:#0008;backdrop-filter:blur(4px)}
#stream-placeholder{padding:40px 16px;text-align:center;color:#444;font-size:.85em}
</style>
</head>
<body>
<header>
  <h1>ESP32-CAM</h1>
  <p>AI Thinker &nbsp;·&nbsp; OV2640 &nbsp;·&nbsp; Plant Inspection</p>
</header>
)";

    // ---- Status + Info grid --------------------------------
    html += "<div class='grid'>";

    // Status tile
    html += "<section><h2>Status</h2>";
    html += "<div class='stat'><span class='k'>Camera</span>"
            "<span class='v'>" + dot(cameraReady) + "&nbsp;" +
            String(cameraReady ? "Ready" : "Failed") + "</span></div>";
    html += "<div class='stat'><span class='k'>Wi-Fi</span>"
            "<span class='v'>" + dot(wifiReady) + "&nbsp;" +
            String(wifiReady ? "Connected" : "Disconnected") + "</span></div>";
    html += "<div class='stat'><span class='k'>PSRAM</span>"
            "<span class='v'>" + dot(hasPsram) + "&nbsp;" +
            String(hasPsram ? "4 MB" : "None") + "</span></div>";
    html += "<div class='stat'><span class='k'>Flash LED</span>"
            "<span class='v' id='flash-stat'>"
            "<span class='" + String(flashOn ? "pill pill-amber" : "pill pill-grey") + "'>" +
            String(flashOn ? "ON" : "OFF") + "</span></span></div>";
    html += "</section>";

    // Info tile
    html += "<section><h2>Network</h2>";
    html += "<div class='stat'><span class='k'>IP</span>"
            "<span class='v green'>" + ip + "</span></div>";
    html += "<div class='stat'><span class='k'>RSSI</span>"
            "<span class='v'>" + rssi + "</span></div>";
    html += "<div class='stat'><span class='k'>Stream res</span>"
            "<span class='v'>" + sResName + "</span></div>";
    html += "<div class='stat'><span class='k'>Capture res</span>"
            "<span class='v green'>" + cResName + "</span></div>";
    html += "<div class='stat'><span class='k'>JPEG quality</span>"
            "<span class='v'>" + String(currentQuality) + "</span></div>";
    html += "</section>";

    html += "</div>"; // end grid

    // ---- Controls ------------------------------------------
    html += "<section style='margin-bottom:12px'><h2>Controls</h2>"
            "<div class='btn-row'>"
            "<button class='btn-primary' onclick='doCapture()'>&#128247; Capture</button>"
            "<button class='btn-success' id='btn-stream' onclick='toggleStream()'>&#9654; Start stream</button>"
            "<button class='btn-amber'   id='btn-flash'  onclick='toggleFlash()'>" +
            String(flashOn ? "&#9728; Flash off" : "&#9728; Flash on") +
            "</button>"
            "</div>"
            "</section>";

    // ---- Resolution ----------------------------------------
    html += "<section style='margin-bottom:12px'><h2>Resolution</h2>"
            "<label>Stream resolution &nbsp;<small>(lower = smoother video)</small>"
            "<select id='sel-sres'>"
            "<option value='QVGA'" + String(sResName=="QVGA"?" selected":"") + ">QVGA &nbsp; 320×240</option>"
            "<option value='VGA'"  + String(sResName=="VGA" ?" selected":"") + ">VGA &nbsp;&nbsp; 640×480</option>"
            "<option value='SVGA'" + String(sResName=="SVGA"?" selected":"") + ">SVGA &nbsp; 800×600</option>"
            "</select></label>"
            "<label style='margin-top:14px'>Capture resolution &nbsp;<small>(higher = sharper photos)</small>"
            "<select id='sel-cres'>"
            "<option value='QVGA'" + String(cResName=="QVGA"?" selected":"") + ">QVGA &nbsp; 320×240</option>"
            "<option value='VGA'"  + String(cResName=="VGA" ?" selected":"") + ">VGA &nbsp;&nbsp; 640×480</option>"
            "<option value='SVGA'" + String(cResName=="SVGA"?" selected":"") + ">SVGA &nbsp; 800×600</option>" +
            String(hasPsram ? "<option value='HD'" + String(cResName=="HD" ?" selected":"") + ">HD &nbsp;&nbsp;&nbsp; 1280×720</option>" : "") +
            String(hasPsram ? "<option value='FHD'" + String(cResName=="FHD"?" selected":"") + ">FHD &nbsp;&nbsp; 1920×1080</option>" : "") +
            "</select></label>"
            "<div class='btn-row' style='margin-top:10px'>"
            "<button class='btn-neutral' onclick='applyStreamRes()'>Apply stream</button>"
            "<button class='btn-neutral' onclick='applyCaptureRes()'>Apply capture</button>"
            "</div>"
            "<p class='feedback' id='res-fb'></p>"
            "</section>";

    // ---- Image quality sliders -----------------------------
    html += "<section style='margin-bottom:12px'><h2>Image Adjustments</h2>"
            "<label>JPEG quality &nbsp;<small>(0 = best file quality, 63 = smallest file)</small>"
            "<input type='number' id='iq' value='" + String(currentQuality) + "' min='0' max='63'></label>"

            "<label>Brightness"
            "<div class='range-row'>"
            "<input type='range' id='ib' min='-2' max='2' value='0' oninput='this.nextElementSibling.textContent=this.value'>"
            "<span class='range-val'>0</span></div></label>"

            "<label>Contrast"
            "<div class='range-row'>"
            "<input type='range' id='ic' min='-2' max='2' value='0' oninput='this.nextElementSibling.textContent=this.value'>"
            "<span class='range-val'>0</span></div></label>"

            "<label>Saturation"
            "<div class='range-row'>"
            "<input type='range' id='is' min='-2' max='2' value='0' oninput='this.nextElementSibling.textContent=this.value'>"
            "<span class='range-val'>0</span></div></label>"

            "<label>Exposure compensation"
            "<div class='range-row'>"
            "<input type='range' id='ia' min='-2' max='2' value='0' oninput='this.nextElementSibling.textContent=this.value'>"
            "<span class='range-val'>0</span></div></label>"

            "<div class='btn-row' style='margin-top:10px'>"
            "<button class='btn-neutral' onclick='applyQuality()'>Apply</button>"
            "</div>"
            "<p class='feedback' id='iq-fb'></p>"
            "</section>";

    // ---- Live stream ---------------------------------------
    html += "<section><h2>Live stream</h2>"
            "<div id='stream-wrap'>"
            "<div id='stream-placeholder'>Stream is not running</div>"
            "<span id='stream-badge' class='pill pill-grey' style='display:none'>Stopped</span>"
            "</div>"
            "</section>";

    // ---- JavaScript ----------------------------------------
    html += "<script>\n"
            "var sImg=null;\n"
            "function toggleStream(){if(sImg)stopStream();else startStream();}\n"
            "function startStream(){\n"
            "  var w=document.getElementById('stream-wrap');\n"
            "  w.innerHTML='<img id=\"simg\" src=\"/stream\" onload=\"onSLoad()\" onerror=\"onSErr()\">'\n"
            "             +'<span id=\"stream-badge\" class=\"pill pill-amber\">Connecting</span>';\n"
            "  sImg=document.getElementById('simg');\n"
            "  document.getElementById('btn-stream').textContent='\\u23F9 Stop stream';\n"
            "  document.getElementById('btn-stream').className='btn-danger';\n"
            "}\n"
            "function stopStream(){\n"
            "  if(sImg){sImg.src='';sImg=null;}\n"
            "  document.getElementById('stream-wrap').innerHTML='<div id=\"stream-placeholder\">Stream stopped</div>';\n"
            "  document.getElementById('btn-stream').textContent='\\u25B6 Start stream';\n"
            "  document.getElementById('btn-stream').className='btn-success';\n"
            "}\n"
            "function onSLoad(){\n"
            "  var b=document.getElementById('stream-badge');\n"
            "  if(b){b.textContent='Live';b.className='pill pill-green';}\n"
            "}\n"
            "function onSErr(){\n"
            "  var b=document.getElementById('stream-badge');\n"
            "  if(b){b.textContent='Disconnected';b.className='pill pill-red';}\n"
            "  sImg=null;\n"
            "  document.getElementById('btn-stream').textContent='\\u25B6 Start stream';\n"
            "  document.getElementById('btn-stream').className='btn-success';\n"
            "}\n"
            "function doCapture(){window.open('/capture','_blank');}\n"
            "var flashState=" + String(flashOn ? "true" : "false") + ";\n"
            "function toggleFlash(){\n"
            "  fetch('/flash?state=toggle').then(function(r){return r.text();}).then(function(t){\n"
            "    flashState=t.indexOf('ON')>=0;\n"
            "    var btn=document.getElementById('btn-flash');\n"
            "    var stat=document.getElementById('flash-stat');\n"
            "    btn.textContent=flashState?'\\u2600 Flash off':'\\u2600 Flash on';\n"
            "    stat.innerHTML=flashState\n"
            "      ?'<span class=\"pill pill-amber\">ON</span>'\n"
            "      :'<span class=\"pill pill-grey\">OFF</span>';\n"
            "  });\n"
            "}\n"
            "function applyStreamRes(){\n"
            "  var r=document.getElementById('sel-sres').value;\n"
            "  fetch('/set_resolution?res='+r).then(function(x){return x.text();}).then(function(t){\n"
            "    document.getElementById('res-fb').textContent=t;\n"
            "    setTimeout(function(){location.reload();},1200);\n"
            "  });\n"
            "}\n"
            "function applyCaptureRes(){\n"
            "  var r=document.getElementById('sel-cres').value;\n"
            "  fetch('/set_capture_res?res='+r).then(function(x){return x.text();}).then(function(t){\n"
            "    document.getElementById('res-fb').textContent=t;\n"
            "    setTimeout(function(){location.reload();},800);\n"
            "  });\n"
            "}\n"
            "function applyQuality(){\n"
            "  var q=document.getElementById('iq').value;\n"
            "  var b=document.getElementById('ib').value;\n"
            "  var c=document.getElementById('ic').value;\n"
            "  var s=document.getElementById('is').value;\n"
            "  var ae=document.getElementById('ia').value;\n"
            "  var url='/set_quality?quality='+q+'&brightness='+b+'&contrast='+c+'&saturation='+s+'&ae_level='+ae;\n"
            "  fetch(url).then(function(x){return x.text();}).then(function(t){\n"
            "    document.getElementById('iq-fb').textContent=t.replace('Updated:\\n','').trim();\n"
            "  });\n"
            "}\n"
            "</script></body></html>";

    return html;
}
