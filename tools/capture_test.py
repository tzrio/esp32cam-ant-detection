#!/usr/bin/env python3
"""
capture_test.py — ESP32-CAM Image Quality Evaluation Tool
==========================================================
Sends HTTP requests to the ESP32-CAM, saves JPEG images with
timestamped filenames, displays them with OpenCV, and logs
capture metrics to a CSV file for resolution comparison.

Usage:
    python3 tools/capture_test.py --ip 10.177.86.91
    python3 tools/capture_test.py --ip 10.177.86.91 --res VGA
    python3 tools/capture_test.py --ip 10.177.86.91 --res VGA --count 5
    python3 tools/capture_test.py --ip 10.177.86.91 --all-res
    python3 tools/capture_test.py --ip 10.177.86.91 --status

Requirements:
    pip install requests opencv-python
"""

import argparse
import csv
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    sys.exit("Missing dependency: pip install requests")

try:
    import cv2
    import numpy as np
    HAS_CV2 = True
except ImportError:
    print("[WARN] opencv-python not installed — images will be saved but not displayed.")
    print("       Install with: pip install opencv-python")
    HAS_CV2 = False

# ---- Default configuration ---------------------------------
DEFAULT_PORT    = 80
DEFAULT_TIMEOUT = 15      # seconds for HTTP request timeout
DEFAULT_COUNT   = 1       # captures per resolution
OUTPUT_DIR      = Path("captures")
CSV_LOG         = OUTPUT_DIR / "results.csv"
DISPLAY_MS      = 3000    # milliseconds to show each captured image

# Resolutions to cycle through in --all-res mode.
# XGA requires PSRAM; the script checks /status before attempting it.
ALL_RESOLUTIONS = ["QVGA", "VGA", "SVGA", "XGA"]

CSV_HEADER = [
    "timestamp", "resolution", "width", "height",
    "jpeg_bytes", "capture_ms", "http_ms",
    "filename", "status"
]


# ============================================================
# HTTP helpers
# ============================================================

def base_url(ip: str, port: int = DEFAULT_PORT) -> str:
    return f"http://{ip}:{port}"


def get_status(ip: str, port: int = DEFAULT_PORT) -> dict | None:
    """Fetch /status JSON from the ESP32-CAM."""
    url = f"{base_url(ip, port)}/status"
    try:
        r = requests.get(url, timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        return json.loads(r.text)
    except requests.exceptions.ConnectionError:
        print(f"[ERR] Cannot connect to {url}")
        print("      Check IP address and that laptop is on the same Wi-Fi.")
        return None
    except Exception as e:
        print(f"[ERR] /status failed: {e}")
        return None


def set_resolution(ip: str, res: str, port: int = DEFAULT_PORT) -> bool:
    """Send GET /set_resolution?res=<RES> and return True on success."""
    url = f"{base_url(ip, port)}/set_resolution?res={res}"
    try:
        r = requests.get(url, timeout=DEFAULT_TIMEOUT)
        if r.status_code == 200:
            print(f"[RES] {r.text.strip()}")
            return True
        else:
            print(f"[ERR] set_resolution HTTP {r.status_code}: {r.text.strip()}")
            return False
    except Exception as e:
        print(f"[ERR] set_resolution failed: {e}")
        return False


def capture_image(ip: str, port: int = DEFAULT_PORT) -> dict | None:
    """
    Send GET /capture.
    Returns a dict with keys:
        data        bytes   raw JPEG
        width       int
        height      int
        jpeg_bytes  int
        capture_ms  int     time reported by ESP32
        http_ms     int     round-trip time measured here
        resolution  str
    Returns None on failure.
    """
    url = f"{base_url(ip, port)}/capture"
    t0  = time.time()
    try:
        r = requests.get(url, timeout=DEFAULT_TIMEOUT)
        http_ms = int((time.time() - t0) * 1000)

        if r.status_code != 200:
            print(f"[ERR] /capture HTTP {r.status_code}: {r.text.strip()}")
            return None

        content_type = r.headers.get("Content-Type", "")
        if "image/jpeg" not in content_type:
            print(f"[ERR] Unexpected Content-Type: {content_type}")
            return None

        # Validate JPEG magic bytes (FF D8 FF)
        data = r.content
        if len(data) < 3 or data[:3] != b'\xff\xd8\xff':
            print(f"[ERR] Response is not a valid JPEG ({len(data)} bytes)")
            return None

        return {
            "data":       data,
            "width":      int(r.headers.get("X-Image-Width",  0)),
            "height":     int(r.headers.get("X-Image-Height", 0)),
            "jpeg_bytes": int(r.headers.get("X-JPEG-Size",    len(data))),
            "capture_ms": int(r.headers.get("X-Capture-Ms",   0)),
            "http_ms":    http_ms,
            "resolution": r.headers.get("X-Resolution", "UNKNOWN"),
        }

    except requests.exceptions.Timeout:
        print(f"[ERR] /capture timed out after {DEFAULT_TIMEOUT}s")
        print("      If streaming is active, stop it first.")
        return None
    except requests.exceptions.ConnectionError:
        print(f"[ERR] Connection refused — is ESP32-CAM running?")
        return None
    except Exception as e:
        print(f"[ERR] Capture failed: {e}")
        return None


# ============================================================
# File helpers
# ============================================================

def save_image(data: bytes, resolution: str, index: int = 0) -> Path:
    """Save JPEG bytes to captures/<timestamp>_<res>_<n>.jpg."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    ts       = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = OUTPUT_DIR / f"{ts}_{resolution}_{index:03d}.jpg"
    filename.write_bytes(data)
    return filename


def append_csv(row: dict) -> None:
    """Append one result row to the CSV log."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    write_header = not CSV_LOG.exists()
    with open(CSV_LOG, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_HEADER)
        if write_header:
            w.writeheader()
        w.writerow(row)


# ============================================================
# Display helper
# ============================================================

def show_image(data: bytes, title: str, wait_ms: int = DISPLAY_MS) -> None:
    """Decode and display JPEG using OpenCV."""
    if not HAS_CV2:
        return
    arr = np.frombuffer(data, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None:
        print("[WARN] OpenCV could not decode image")
        return
    # Resize for display if larger than 800px wide
    h, w = img.shape[:2]
    if w > 800:
        scale = 800 / w
        img   = cv2.resize(img, (800, int(h * scale)))
    cv2.imshow(title, img)
    cv2.waitKey(wait_ms)
    cv2.destroyAllWindows()


# ============================================================
# Core test routine
# ============================================================

def run_capture_test(ip: str, resolution: str | None,
                     count: int, port: int = DEFAULT_PORT) -> None:
    """
    Set resolution (if given), capture `count` images, save, display, log.
    """
    if resolution:
        print(f"\n[TEST] Setting resolution to {resolution}...")
        if not set_resolution(ip, resolution, port):
            print(f"[SKIP] Skipping {resolution} — could not set resolution")
            return
        time.sleep(0.5)  # give sensor time to settle

    for i in range(count):
        print(f"\n[CAP]  Capturing image {i+1}/{count}...")
        result = capture_image(ip, port)

        if result is None:
            row = {
                "timestamp":   datetime.now().isoformat(),
                "resolution":  resolution or "UNKNOWN",
                "width":       0, "height": 0,
                "jpeg_bytes":  0, "capture_ms": 0, "http_ms": 0,
                "filename":    "", "status": "FAIL"
            }
            append_csv(row)
            continue

        filename = save_image(result["data"], result["resolution"], i)

        row = {
            "timestamp":   datetime.now().isoformat(),
            "resolution":  result["resolution"],
            "width":       result["width"],
            "height":      result["height"],
            "jpeg_bytes":  result["jpeg_bytes"],
            "capture_ms":  result["capture_ms"],
            "http_ms":     result["http_ms"],
            "filename":    str(filename),
            "status":      "OK"
        }
        append_csv(row)

        print(f"[OK]   {result['resolution']}  "
              f"{result['width']}x{result['height']}  "
              f"{result['jpeg_bytes']:,} bytes  "
              f"capture={result['capture_ms']} ms  "
              f"http={result['http_ms']} ms")
        print(f"[SAVE] {filename}")

        show_image(result["data"],
                   f"{result['resolution']} {result['width']}x{result['height']} "
                   f"— {result['jpeg_bytes']:,} bytes")

        if i < count - 1:
            time.sleep(0.3)  # brief pause between consecutive captures


def run_all_resolutions(ip: str, count: int,
                        port: int = DEFAULT_PORT) -> None:
    """Cycle through all resolutions, capturing `count` images each."""
    # Check PSRAM before attempting XGA
    status = get_status(ip, port)
    has_psram = status.get("psram", False) if status else False

    for res in ALL_RESOLUTIONS:
        if res == "XGA" and not has_psram:
            print(f"\n[SKIP] {res} skipped — requires PSRAM (not detected)")
            continue
        run_capture_test(ip, res, count, port)

    # Restore to QVGA after test sequence
    print("\n[RES] Restoring to QVGA...")
    set_resolution(ip, "QVGA", port)

    print(f"\n[LOG]  Results saved to {CSV_LOG}")
    print_summary()


def print_summary() -> None:
    """Print a formatted results table from the CSV log."""
    if not CSV_LOG.exists():
        print("[INFO] No results log found.")
        return

    print("\n" + "=" * 72)
    print(f"  {'Resolution':<8} {'Size':>10}  {'W×H':<12} "
          f"{'Capture':>10}  {'HTTP':>8}  Status")
    print("=" * 72)

    with open(CSV_LOG, newline="") as f:
        for row in csv.DictReader(f):
            dims = (f"{row['width']}×{row['height']}"
                    if row['width'] != '0' else "—")
            size = (f"{int(row['jpeg_bytes']):,} B"
                    if row['jpeg_bytes'] != '0' else "—")
            print(f"  {row['resolution']:<8} {size:>10}  {dims:<12} "
                  f"{row['capture_ms']:>8} ms  {row['http_ms']:>6} ms  "
                  f"{row['status']}")
    print("=" * 72)


# ============================================================
# Entry point
# ============================================================

def main() -> None:
    parser = argparse.ArgumentParser(
        description="ESP32-CAM image quality capture and evaluation tool")

    parser.add_argument("--ip",      required=True,
                        help="ESP32-CAM IP address (e.g. 10.177.86.91)")
    parser.add_argument("--port",    type=int, default=DEFAULT_PORT,
                        help=f"HTTP port (default: {DEFAULT_PORT})")
    parser.add_argument("--res",     choices=["QVGA","VGA","SVGA","XGA"],
                        help="Resolution to test (QVGA/VGA/SVGA/XGA)")
    parser.add_argument("--count",   type=int, default=DEFAULT_COUNT,
                        help=f"Number of images to capture (default: {DEFAULT_COUNT})")
    parser.add_argument("--all-res", action="store_true",
                        help="Test all resolutions sequentially")
    parser.add_argument("--status",  action="store_true",
                        help="Print /status JSON and exit")
    parser.add_argument("--summary", action="store_true",
                        help="Print results summary from CSV log and exit")

    args = parser.parse_args()

    # -- Summary only -----------------------------------------
    if args.summary:
        print_summary()
        return

    # -- Status only ------------------------------------------
    if args.status:
        print(f"[INFO] Fetching status from {args.ip}:{args.port}...")
        s = get_status(args.ip, args.port)
        if s:
            print(json.dumps(s, indent=2))
        return

    # -- Connectivity check -----------------------------------
    print(f"[INFO] Connecting to ESP32-CAM at {args.ip}:{args.port}...")
    status = get_status(args.ip, args.port)
    if status is None:
        sys.exit(1)

    print(f"[INFO] Camera ready : {status.get('camera_ready')}")
    print(f"[INFO] Resolution   : {status.get('resolution')}")
    print(f"[INFO] PSRAM        : {status.get('psram')}")
    print(f"[INFO] Free heap    : {status.get('free_heap'):,} bytes")

    if not status.get("camera_ready"):
        sys.exit("[ERR] Camera not ready on ESP32-CAM. Check serial monitor.")

    if status.get("stream_active"):
        print("[WARN] Stream is active on ESP32-CAM.")
        print("       Stop the stream in the browser before capturing.")
        sys.exit(1)

    # -- Run test ---------------------------------------------
    if args.all_res:
        run_all_resolutions(args.ip, args.count, args.port)
    else:
        run_capture_test(args.ip, args.res, args.count, args.port)
        print(f"\n[LOG]  Results saved to {CSV_LOG}")
        print_summary()


if __name__ == "__main__":
    main()
