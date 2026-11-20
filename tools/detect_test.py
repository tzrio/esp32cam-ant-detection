#!/usr/bin/env python3
"""
detect_test.py — Ant Detection Testing Tool
============================================
Ambil foto dari ESP32-CAM, jalankan inferensi YOLOv8,
tampilkan hasilnya dengan bounding box.

Usage:
    # Test satu capture dari ESP32-CAM
    python3 tools/detect_test.py --ip 10.177.86.91

    # Test dari file foto lokal
    python3 tools/detect_test.py --file foto.jpg

    # Live detection dari stream ESP32-CAM
    python3 tools/detect_test.py --ip 10.177.86.91 --live

    # Ganti model path
    python3 tools/detect_test.py --ip 10.177.86.91 --model tools/ant_detection_best.pt

Requirements:
    pip install ultralytics requests opencv-python
"""

import argparse
import sys
import time
from pathlib import Path

# Use system OpenCV (GTK GUI support) instead of pip headless version
sys.path.insert(0, '/usr/lib/python3/dist-packages')

# ---- Dependency checks -------------------------------------
try:
    import cv2
    import numpy as np
except ImportError:
    sys.exit("[ERR] pip install opencv-python")

try:
    from ultralytics import YOLO
except ImportError:
    sys.exit("[ERR] pip install ultralytics")

try:
    import requests
except ImportError:
    sys.exit("[ERR] pip install requests")

# ---- Default paths -----------------------------------------
SCRIPT_DIR  = Path(__file__).parent
DEFAULT_MODEL = SCRIPT_DIR / "ant_detection_best.pt"
DEFAULT_CONF  = 0.3   # minimum confidence threshold


# ============================================================
# Fetch single frame from ESP32-CAM /capture
# ============================================================
def fetch_frame(ip: str, port: int = 80, timeout: int = 10):
    url = f"http://{ip}:{port}/capture"
    try:
        r = requests.get(url, timeout=timeout)
        if r.status_code != 200:
            print(f"[ERR] /capture returned HTTP {r.status_code}: {r.text.strip()}")
            return None
        data = r.content
        if data[:3] != b'\xff\xd8\xff':
            print("[ERR] Response is not a valid JPEG")
            return None
        arr = np.frombuffer(data, np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        return img
    except requests.exceptions.Timeout:
        print(f"[ERR] Timeout — is stream active? Stop it first.")
        return None
    except requests.exceptions.ConnectionError:
        print(f"[ERR] Cannot connect to {ip}:{port}")
        return None


# ============================================================
# Run YOLO inference and annotate image
# ============================================================
def run_inference(model: YOLO, img, conf: float = DEFAULT_CONF):
    t0      = time.time()
    results = model(img, conf=conf, verbose=False)
    ms      = (time.time() - t0) * 1000

    result     = results[0]
    annotated  = result.plot()   # BGR image with boxes drawn
    detections = []

    for box in result.boxes:
        detections.append({
            "class":      model.names[int(box.cls[0])],
            "confidence": float(box.conf[0]),
            "bbox":       box.xyxy[0].tolist(),  # [x1,y1,x2,y2]
        })

    return annotated, detections, ms


# ============================================================
# Print detection summary to terminal
# ============================================================
def print_summary(detections: list, infer_ms: float, source: str = ""):
    if source:
        print(f"\n[SRC] {source}")
    print(f"[INF] Inference: {infer_ms:.1f} ms")
    if not detections:
        print("[DET] No ants detected")
    else:
        print(f"[DET] {len(detections)} ant(s) detected:")
        for i, d in enumerate(detections, 1):
            x1, y1, x2, y2 = [int(v) for v in d["bbox"]]
            w = x2 - x1
            h = y2 - y1
            print(f"      [{i}] conf={d['confidence']:.2f}  "
                  f"bbox=({x1},{y1}) {w}×{h}px")


# ============================================================
# Single capture mode
# ============================================================
def mode_single(args):
    print(f"[INF] Loading model: {args.model}")
    model = YOLO(str(args.model))

    if args.file:
        img = cv2.imread(args.file)
        if img is None:
            sys.exit(f"[ERR] Cannot read file: {args.file}")
        source = args.file
    else:
        print(f"[NET] Fetching frame from {args.ip}...")
        img = fetch_frame(args.ip, args.port)
        if img is None:
            sys.exit(1)
        source = f"http://{args.ip}/capture"

    annotated, detections, ms = run_inference(model, img, args.conf)
    print_summary(detections, ms, source)

    # Save annotated image
    out_path = Path("captures") / f"detect_{int(time.time())}.jpg"
    out_path.parent.mkdir(exist_ok=True)
    cv2.imwrite(str(out_path), annotated)
    print(f"[SAV] Saved: {out_path}")

    # Display in window
    cv2.imshow("Ant Detection", annotated)
    print("[UI]  Press any key to close")
    cv2.waitKey(0)
    cv2.destroyAllWindows()


# ============================================================
# Live mode — repeatedly fetch /capture and infer
# ============================================================
def mode_live(args):
    print(f"[INF] Loading model: {args.model}")
    model = YOLO(str(args.model))
    print(f"[NET] Live detection from {args.ip}")
    print("[UI]  Press Q in the window to quit")

    frame_count = 0
    ant_frames  = 0

    while True:
        img = fetch_frame(args.ip, args.port, timeout=5)
        if img is None:
            time.sleep(0.5)
            continue

        annotated, detections, ms = run_inference(model, img, args.conf)
        frame_count += 1
        if detections:
            ant_frames += 1

        # Overlay stats on frame
        status   = f"ANT DETECTED ({len(detections)})" if detections else "No ant"
        color    = (0, 255, 0) if detections else (100, 100, 100)
        cv2.putText(annotated, status,  (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)
        cv2.putText(annotated, f"{ms:.0f}ms  frame {frame_count}",
                    (10, annotated.shape[0] - 10),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1)

        cv2.imshow("Ant Detection — Live", annotated)

        if detections:
            out_path = Path("captures") / f"live_{frame_count:04d}.jpg"
            out_path.parent.mkdir(exist_ok=True)
            cv2.imwrite(str(out_path), annotated)
            print(f"[DET] Frame {frame_count}: "
                  f"{len(detections)} ant(s)  "
                  f"conf={max(d['confidence'] for d in detections):.2f}  "
                  f"{ms:.0f}ms  → {out_path}")
        else:
            print(f"[   ] Frame {frame_count}: no ant  {ms:.0f}ms")

        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    cv2.destroyAllWindows()
    print(f"\n[SUM] {frame_count} frames  |  "
          f"{ant_frames} with ant  |  "
          f"{ant_frames/max(frame_count,1)*100:.1f}% detection rate")


# ============================================================
# Entry point
# ============================================================
def main():
    parser = argparse.ArgumentParser(
        description="Ant detection testing tool for ESP32-CAM")

    parser.add_argument("--ip",    default="10.177.86.91",
                        help="ESP32-CAM IP address")
    parser.add_argument("--port",  type=int, default=80)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL,
                        help=f"Path to .pt model (default: {DEFAULT_MODEL})")
    parser.add_argument("--conf",  type=float, default=DEFAULT_CONF,
                        help=f"Confidence threshold (default: {DEFAULT_CONF})")
    parser.add_argument("--file",  type=str, default=None,
                        help="Test from a local image file instead of ESP32-CAM")
    parser.add_argument("--live",  action="store_true",
                        help="Continuous live detection mode")

    args = parser.parse_args()

    if not Path(args.model).exists():
        sys.exit(f"[ERR] Model not found: {args.model}\n"
                 f"      Place best.pt in tools/ or use --model /path/to/model.pt")

    if args.live and not args.file:
        mode_live(args)
    else:
        mode_single(args)


if __name__ == "__main__":
    main()
