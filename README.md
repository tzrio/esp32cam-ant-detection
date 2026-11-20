# ESP32-CAM Ant Detection

Plant inspection robot camera system using **AI Thinker ESP32-CAM** (OV5640) with real-time ant detection via **YOLOv8n**.

Built for a Microprocessor & Interface course project — Telkom University.

---

## Features

- OV5640 camera initialisation with runtime PSRAM detection
- Wi-Fi connection with reconnect handling
- HTTP web server running directly on the ESP32-CAM:
  - `GET /` — camera control & status webpage
  - `GET /capture` — single JPEG still image
  - `GET /stream` — MJPEG live stream (FreeRTOS core 1, non-blocking)
  - `GET /set_resolution` — change stream resolution at runtime
  - `GET /set_capture_res` — change capture resolution at runtime
  - `GET /set_quality` — adjust JPEG quality, brightness, contrast, saturation, exposure
  - `GET /flash` — toggle onboard flash LED (GPIO 4)
  - `GET /status` — JSON system status
- Python tools running on the laptop:
  - `capture_test.py` — capture images, log metrics to CSV
  - `detect_test.py` — single-frame or live ant detection with bounding boxes (cv2 window)
  - `detect_server.py` — Flask web server serving live MJPEG with YOLO overlay in browser

---

## Hardware

| Component | Part |
|-----------|------|
| Module | AI Thinker ESP32-CAM |
| Sensor | OV5640 (PID 0x5640, confirmed via ribbon cable label) |
| Programmer | ESP32-CAM-MB (CH340 built-in) |

---

## Project Structure

```
esp32cam-ant-detection/
├── platformio.ini          # PlatformIO board, framework, upload config
├── src/
│   └── main.cpp            # Firmware — camera init, Wi-Fi, HTTP server
├── include/
│   └── config.h            # Wi-Fi credentials and all configurable parameters
├── lib/                    # Empty — no external libraries required
├── dataset/                # Raw image dataset (tracked via Git LFS)
│   └── 20 Nov 2026/        # 185 images, renamed 001.png–185.png
├── tools/
│   ├── capture_test.py     # Image capture + quality evaluation tool
│   ├── detect_test.py      # Local or live ant detection (cv2 window)
│   └── detect_server.py    # Flask server — browser-based live detection
└── README.md
```

> **Not tracked by git:** `captures/`, `.pio/`, `tools/*.pt`

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

> Use a **2.4 GHz** network. The ESP32 does not support 5 GHz.  
> Captive portal networks (university/hotel login pages) will not work — use a personal hotspot.

### 3. Find the Serial Port (Linux)

```bash
ls /dev/ttyUSB*
```

If permission is denied on upload:

```bash
sudo usermod -aG dialout $USER
# Log out and back in
```

---

## Build & Upload

```bash
cd esp32cam-ant-detection

# Build only
pio run

# Build and upload
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor
```

After boot, the serial monitor shows:

```
[CAM] Init OK — stream: QVGA  capture: QVGA
[NET] Connected — IP: 192.168.x.x  RSSI: -xx dBm
[HTTP] Listening on port 80
[HTTP] /  http://192.168.x.x/
```

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
- Start / stop the MJPEG stream

---

## Python Tools

> Make sure the laptop is on the **same Wi-Fi network** as the ESP32-CAM.

### Capture a single image

```bash
python3 tools/capture_test.py --ip <ESP32_IP>
```

### Test ant detection from a local file

```bash
python3 tools/detect_test.py --file "dataset/20 Nov 2026/001.png"
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

## Dataset

### Raw images (this repository)

185 images captured with the ESP32-CAM OV5640, stored in `dataset/20 Nov 2026/` and tracked via **Git LFS**.

Images are named sequentially (`001.png` – `185.png`) sorted by capture timestamp.

To clone including dataset files:

```bash
git lfs install
git clone https://github.com/tzrio/esp32cam-ant-detection.git
```

### Labelled dataset (Roboflow)

The labelled and augmented dataset is available on Roboflow:

**[ant-detection on Roboflow](https://app.roboflow.com/roihans-workspace-mwhah/ant-detection-rp6yc/1)**

| Property | Value |
|----------|-------|
| Images (raw) | 185 |
| Images (after 3× augmentation) | 338 |
| Classes | `ant` |
| Split | 92% train / 4% valid / 4% test |
| Augmentations | Flip horizontal, Brightness ±20%, Blur up to 2px, Noise up to 2% |
| Labelling method | Auto-label (Roboflow GPT-4.1 Terra) + manual review |

---

## YOLOv8 Training

### Environment

Training was performed on **Google Colab** with Tesla T4 GPU (free tier).

### Steps

1. Upload dataset to Roboflow → auto-label with class `ant` → review → export as YOLOv8 format
2. Open Google Colab → Runtime → Change runtime type → **T4 GPU**
3. Run the training notebook:

```python
from roboflow import Roboflow
from ultralytics import YOLO

# Download dataset from Roboflow
rf = Roboflow(api_key="YOUR_API_KEY")
project = rf.workspace("roihans-workspace-mwhah").project("ant-detection-rp6yc")
dataset = project.version(1).download("yolov8")

# Train
model = YOLO("yolov8n.pt")
model.train(
    data=f"{dataset.location}/data.yaml",
    epochs=100,
    imgsz=512,
    batch=16,
    patience=20,       # early stopping
    device=0,
)
```

Training stopped early at **epoch 44** (best at epoch 24) due to early stopping — no improvement in 20 consecutive epochs.

### Results

| Metric | Value |
|--------|-------|
| Architecture | YOLOv8n (nano) |
| Epochs trained | 44 / 100 (early stop) |
| Best epoch | 24 |
| mAP50 | **0.995** |
| Precision | **0.982** |
| Recall | **1.000** |
| Inference (CPU laptop) | ~400 ms/frame |
| Inference (Tesla T4 GPU) | ~3 ms/frame |

### Model Weights

`tools/ant_detection_best.pt` is **not included** in this repository (excluded by `.gitignore`).

Download `ant_detection_best.pt` from the [**Releases page**](../../releases) and place it in the `tools/` folder:

```
tools/
└── ant_detection_best.pt   ← place here
```

---

## Architecture

```
┌──────────────────────────────────────┐
│  ESP32-CAM (OV5640)                  │
│  Firmware: src/main.cpp              │
│                                      │
│  GET /  /capture  /stream  /status   │
└──────────────────┬───────────────────┘
                   │ Wi-Fi (2.4 GHz)
       ┌───────────┴────────────┐
       │                        │
┌──────▼──────┐      ┌──────────▼──────────────┐
│ Browser     │      │ Laptop — Python          │
│             │      │                          │
│ http://     │      │ detect_server.py         │
│ <ESP_IP>/   │      │ ├─ fetch /capture        │
│             │      │ ├─ YOLO inference        │
│ Raw stream  │      │ └─ Flask MJPEG + bbox    │
│ Camera      │      │    → localhost:5000      │
│ config UI   │      │    → <LAPTOP_IP>:5000    │
└─────────────┘      └──────────────────────────┘
```

---

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Upload fails "Failed to connect" | Not in flash mode | Hold BOOT then press RESET on MB board |
| Serial monitor shows nothing | Need to press reset after monitor opens | Press RESET button after `pio device monitor` starts |
| Camera init fails `0x105` | Ribbon cable loose | Re-seat camera ribbon cable with board powered off |
| Stream freezes / `EV-EOF-OVF` | Resolution too high | Use QVGA for streaming |
| Purple/pink tint in images | IR bleed — no IR cut filter | Use white front-lighting; increase saturation via `/set_quality` |
| `localhost:5000` loads slowly | YOLO inference occupies CPU | Expected on CPU-only (~2-3 fps); use GPU for real-time |
| `equal_valued` ImportError | sympy/torch version conflict | `pip install torchvision==0.16.2 --index-url https://download.pytorch.org/whl/cpu --force-reinstall` |
