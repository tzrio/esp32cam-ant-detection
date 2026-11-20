# ESP32-CAM Ant Detection

Plant inspection robot camera system using **AI Thinker ESP32-CAM** (OV5640) with real-time ant detection via **YOLOv8n**.

Built for a Microprocessor & Interface course project — Telkom University.

---

## Features

- OV5640 camera initialisation with runtime PSRAM detection
- Wi-Fi connection with reconnect handling
- HTTP web server on the ESP32-CAM:
  - `GET /` — camera control & status webpage
  - `GET /capture` — single high-resolution JPEG (up to FHD 1920×1080)
  - `GET /stream` — MJPEG live stream (QVGA default, runs on FreeRTOS core 1)
  - `GET /set_resolution` — change stream resolution at runtime
  - `GET /set_capture_res` — change capture resolution at runtime
  - `GET /set_quality` — adjust JPEG quality, brightness, contrast, saturation
  - `GET /flash` — toggle onboard flash LED
  - `GET /status` — JSON system status
- Python tools on the laptop:
  - `capture_test.py` — capture images, log metrics to CSV
  - `detect_test.py` — single-frame or live ant detection with bounding boxes
  - `detect_server.py` — Flask web server serving live MJPEG with YOLO overlay

---

## Hardware

| Component | Part |
|-----------|------|
| Module | AI Thinker ESP32-CAM |
| Sensor | OV5640 (5MP, detected as PID 0x5640) |
| Programmer | ESP32-CAM-MB (CH340 built-in) |

---

## Project Structure

```
esp32cam-test/
├── platformio.ini          # PlatformIO board, framework, upload config
├── src/
│   └── main.cpp            # Firmware — camera, Wi-Fi, HTTP server
├── include/
│   └── config.h            # Wi-Fi credentials and all configurable parameters
├── lib/                    # Empty — no external libraries required
├── tools/
│   ├── capture_test.py     # Image capture + quality evaluation
│   ├── detect_test.py      # Local or live ant detection (cv2 window)
│   └── detect_server.py    # Flask server — browser-based live detection
└── README.md
```

> **Not tracked by git:** `captures/`, `dataset/`, `tools/*.pt`, `.pio/`

---

## Setup

### 1. Required Software

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
- Python 3.10+
- Python packages:

```bash
pip install ultralytics requests flask
sudo apt install python3-opencv   # Ubuntu/Debian — for cv2 GUI support
```

### 2. Configure Wi-Fi

Edit `include/config.h`:

```cpp
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

Use a **2.4 GHz** network. The ESP32 does not support 5 GHz.  
Captive portal networks (university/hotel login pages) will not work — use a personal hotspot.

### 3. Find the Serial Port (Linux)

```bash
ls /dev/ttyUSB*
```

If permission denied on upload:

```bash
sudo usermod -aG dialout $USER
# Log out and back in
```

---

## Build & Upload

```bash
cd esp32cam-test

# Build only
pio run

# Build and upload
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor
```

After boot, the serial monitor shows:

```
[CAM] Init OK — stream: QVGA  capture: FHD
[NET] Connected — IP: 192.168.x.x  RSSI: -xx dBm
[HTTP] Listening on port 80
```

---

## Entering Flash Mode

The ESP32-CAM-MB handles this automatically via the CH340 on the board.  
If upload fails with "Failed to connect":

1. Hold the BOOT button on the MB board
2. Press and release RESET
3. Release BOOT
4. Run `pio run --target upload`

---

## Accessing the Camera Web Interface

After the IP address appears in the serial monitor, open a browser on the **same network**:

```
http://<IP_ADDRESS>/
```

From here you can:
- View camera and Wi-Fi status
- Change stream resolution (QVGA / VGA / SVGA)
- Change capture resolution (QVGA / VGA / SVGA / HD / FHD)
- Adjust brightness, contrast, saturation, exposure
- Toggle the flash LED
- Capture a still image
- Start/stop the MJPEG stream

---

## Python Tools

> Make sure the laptop is on the **same Wi-Fi network** as the ESP32-CAM.

### Capture a single image

```bash
python3 tools/capture_test.py --ip <ESP32_IP>
```

### Test ant detection from a local file

```bash
python3 tools/detect_test.py --file dataset/001.png
```

### Live ant detection (cv2 window)

```bash
python3 tools/detect_test.py --ip <ESP32_IP> --live
```

Press **Q** in the window to quit.

### Browser-based live detection server

```bash
python3 tools/detect_server.py --ip <ESP32_IP>
```

Then open:
- Laptop: `http://localhost:5000`
- Phone (same network): `http://<LAPTOP_IP>:5000`

---

## YOLOv8 Model

The ant detection model (`tools/ant_detection_best.pt`) is **not included** in this repository due to file size.

| Property | Value |
|----------|-------|
| Architecture | YOLOv8n (nano) |
| Classes | `ant` |
| Training dataset | 185 images → 338 after augmentation |
| mAP50 | 0.995 |
| Precision | 0.982 |
| Recall | 1.000 |
| Input size | 512×512 |

Training was done on Google Colab (Tesla T4 GPU) using [Roboflow](https://roboflow.com) for labelling and augmentation.

To obtain the model weights, either:
- Download from the [Releases](../../releases) page of this repository
- Re-train using the Colab notebook in `tools/` (see `capture_test.py` for dataset structure)

---

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Upload fails "Failed to connect" | Not in flash mode | Hold BOOT then press RESET |
| Serial monitor shows nothing | DTR/RTS holding GPIO0 low | `monitor_dtr=0` and `monitor_rts=0` already set in `platformio.ini` — press RESET after monitor opens |
| Camera init fails `0x105` | Ribbon cable loose | Re-seat camera ribbon cable with board powered off |
| Stream freezes / `EV-EOF-OVF` | Resolution too high for Wi-Fi | Use QVGA for streaming; use SVGA/HD/FHD for capture only |
| Purple/pink tint in images | IR bleed — OV5640 lacks IR cut filter | Use white front-lighting; adjust saturation via `/set_quality` |
| `localhost:5000` loads slowly | YOLO inference occupies CPU | Expected on CPU-only laptop (~2-3 fps); upgrade to GPU machine for real-time |
| `equal_valued` ImportError | sympy version conflict | `pip install sympy==1.13.3 --force-reinstall` then downgrade torchvision |

---

## Architecture

```
┌──────────────────────────────────┐
│  ESP32-CAM (OV5640)              │
│  Firmware: main.cpp              │
│                                  │
│  / capture / stream / status ... │
└──────────────┬───────────────────┘
               │ Wi-Fi
   ┌───────────┴────────────┐
   │                        │
┌──▼─────────┐    ┌─────────▼──────────────┐
│ Browser    │    │ Laptop (Python)         │
│ direct     │    │ detect_server.py        │
│            │    │                         │
│ Raw stream │    │ YOLO inference          │
│ no YOLO    │    │ → Flask MJPEG + bbox    │
│            │    │ → localhost:5000        │
└────────────┘    └─────────────────────────┘
```
