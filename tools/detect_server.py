#!/usr/bin/env python3
"""
detect_server.py — Flask Detection Server
==========================================
Jalankan YOLO inferensi dari frame ESP32-CAM lalu
serve hasilnya sebagai MJPEG stream ke browser.

Usage:
    python3 tools/detect_server.py --ip 10.219.3.171

Buka browser:
    http://localhost:5000

Bisa dibuka dari HP juga (satu jaringan):
    http://<IP_LAPTOP>:5000
"""

import argparse
import sys
import time
import threading
from pathlib import Path

# Use system OpenCV (GTK/GUI support)
sys.path.insert(0, '/usr/lib/python3/dist-packages')

try:
    import cv2
    import numpy as np
except ImportError:
    sys.exit("[ERR] pip install opencv-python")

try:
    import requests
except ImportError:
    sys.exit("[ERR] pip install requests")

try:
    from ultralytics import YOLO
except ImportError:
    sys.exit("[ERR] pip install ultralytics")

try:
    from flask import Flask, Response, render_template_string
except ImportError:
    sys.exit("[ERR] pip install flask")

# ---- Config ------------------------------------------------
SCRIPT_DIR    = Path(__file__).parent
DEFAULT_MODEL = SCRIPT_DIR / "ant_detection_best.pt"
DEFAULT_CONF  = 0.3
FLASK_PORT    = 5000

# ---- Globals -----------------------------------------------
app          = Flask(__name__)
frame_lock   = threading.Lock()
latest_frame = None   # latest annotated JPEG bytes
stats        = {
    "frame":    0,
    "ants":     0,
    "infer_ms": 0,
    "fps":      0.0,
}

# ---- HTML template -----------------------------------------
HTML = """
<!DOCTYPE html>
<html lang='en'>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width,initial-scale=1'>
  <title>Ant Detection</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { background: #0f0f0f; color: #e5e5e5;
           font-family: -apple-system, sans-serif; padding: 16px; }
    header { padding-bottom: 12px; border-bottom: 1px solid #2a2a2a;
             margin-bottom: 16px; }
    header h1 { font-size: 1.2em; font-weight: 600; }
    header p  { font-size: .8em; color: #666; margin-top: 3px; }
    .layout { display: grid; grid-template-columns: 1fr 280px; gap: 16px; }
    @media(max-width:700px){ .layout { grid-template-columns: 1fr; } }
    .stream-wrap { background: #111; border-radius: 10px; overflow: hidden;
                   border: 1px solid #2a2a2a; }
    .stream-wrap img { width: 100%; display: block; }
    .no-frame { padding: 60px 20px; text-align: center; color: #444; }
    .sidebar section { background: #1a1a1a; border: 1px solid #2a2a2a;
                       border-radius: 10px; padding: 14px; margin-bottom: 12px; }
    .sidebar h2 { font-size: .72em; font-weight: 600; letter-spacing: .08em;
                  text-transform: uppercase; color: #555; margin-bottom: 10px; }
    .stat { display: flex; justify-content: space-between;
            padding: 5px 0; border-bottom: 1px solid #222; font-size: .85em; }
    .stat:last-child { border-bottom: none; }
    .k { color: #777; }
    .v { font-weight: 500; }
    .green  { color: #34d399; }
    .red    { color: #f87171; }
    .amber  { color: #fbbf24; }
    #ant-status { font-size: 1.1em; font-weight: 700; padding: 10px;
                  border-radius: 8px; text-align: center; margin-top: 4px;
                  background: #052e16; color: #34d399; }
    #ant-status.detected { background: #450a0a; color: #f87171; }
  </style>
</head>
<body>
<header>
  <h1>Ant Detection — Live</h1>
  <p>ESP32-CAM &nbsp;·&nbsp; YOLOv8n &nbsp;·&nbsp; Real-time inference</p>
</header>

<div class='layout'>
  <div class='stream-wrap'>
    <img id='feed' src='/video_feed' alt='stream'>
  </div>

  <div class='sidebar'>
    <section>
      <h2>Detection</h2>
      <div id='ant-status'>Loading...</div>
    </section>

    <section>
      <h2>Stats</h2>
      <div class='stat'><span class='k'>Frame</span>
           <span class='v' id='s-frame'>—</span></div>
      <div class='stat'><span class='k'>Ants detected</span>
           <span class='v green' id='s-ants'>—</span></div>
      <div class='stat'><span class='k'>Inference</span>
           <span class='v' id='s-infer'>—</span></div>
      <div class='stat'><span class='k'>FPS</span>
           <span class='v' id='s-fps'>—</span></div>
    </section>

    <section>
      <h2>Source</h2>
      <div class='stat'><span class='k'>Camera IP</span>
           <span class='v green' id='s-ip'>{{ ip }}</span></div>
      <div class='stat'><span class='k'>Model</span>
           <span class='v'>YOLOv8n</span></div>
      <div class='stat'><span class='k'>Confidence</span>
           <span class='v'>{{ conf }}</span></div>
    </section>
  </div>
</div>

<script>
// Poll /stats every second to update sidebar
function updateStats() {
  fetch('/stats').then(r => r.json()).then(d => {
    document.getElementById('s-frame').textContent = d.frame;
    document.getElementById('s-ants').textContent  = d.ants;
    document.getElementById('s-infer').textContent = d.infer_ms + ' ms';
    document.getElementById('s-fps').textContent   = d.fps.toFixed(1) + ' fps';

    var box = document.getElementById('ant-status');
    if (d.ants > 0) {
      box.textContent = '🐜 ANT DETECTED (' + d.ants + ')';
      box.className = 'detected';
    } else {
      box.textContent = '✅ No ant detected';
      box.className = '';
    }
  }).catch(() => {});
}
setInterval(updateStats, 1000);
updateStats();
</script>
</body>
</html>
"""

# ============================================================
# Capture frame from ESP32-CAM
# ============================================================
def fetch_frame(ip: str, port: int = 80, timeout: int = 8):
    try:
        r = requests.get(f"http://{ip}:{port}/capture", timeout=timeout)
        if r.status_code != 200:
            return None
        data = r.content
        if data[:3] != b'\xff\xd8\xff':
            return None
        arr = np.frombuffer(data, np.uint8)
        return cv2.imdecode(arr, cv2.IMREAD_COLOR)
    except Exception:
        return None


# ============================================================
# Background thread: continuously fetch + infer
# ============================================================
def detection_loop(ip: str, port: int, model: YOLO, conf: float):
    global latest_frame, stats

    fps_timer  = time.time()
    fps_frames = 0

    print(f"[LOOP] Detection loop started — {ip}:{port}")

    while True:
        img = fetch_frame(ip, port)
        if img is None:
            time.sleep(0.3)
            continue

        # YOLO inference
        t0      = time.time()
        results = model(img, conf=conf, verbose=False)
        infer_ms = int((time.time() - t0) * 1000)

        result    = results[0]
        annotated = result.plot()
        n_ants    = len(result.boxes)

        # Encode annotated frame to JPEG
        _, buf = cv2.imencode('.jpg', annotated,
                              [cv2.IMWRITE_JPEG_QUALITY, 80])

        # FPS calculation
        fps_frames += 1
        elapsed = time.time() - fps_timer
        if elapsed >= 2.0:
            fps        = fps_frames / elapsed
            fps_timer  = time.time()
            fps_frames = 0
        else:
            fps = stats["fps"]

        with frame_lock:
            latest_frame = buf.tobytes()
            stats["frame"]    += 1
            stats["ants"]      = n_ants
            stats["infer_ms"]  = infer_ms
            stats["fps"]       = round(fps, 1)

        if n_ants > 0:
            print(f"[DET] Frame {stats['frame']}: "
                  f"{n_ants} ant(s)  conf="
                  f"{max(float(b.conf[0]) for b in result.boxes):.2f}  "
                  f"{infer_ms}ms")

        # Yield CPU briefly so Flask can serve HTML/stats requests
        time.sleep(0.05)


# ============================================================
# Flask routes
# ============================================================
@app.route('/')
def index():
    from flask import request as freq
    ip   = app.config['ESP_IP']
    conf = app.config['CONF']
    return render_template_string(HTML, ip=ip, conf=conf)


def generate_mjpeg():
    """Yield MJPEG frames for the browser."""
    while True:
        with frame_lock:
            frame = latest_frame

        if frame is None:
            # Send a small placeholder while waiting for first frame
            time.sleep(0.1)
            continue

        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' +
               frame + b'\r\n')


@app.route('/video_feed')
def video_feed():
    return Response(generate_mjpeg(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')


@app.route('/stats')
def get_stats():
    from flask import jsonify
    with frame_lock:
        s = dict(stats)
    return jsonify(s)


# ============================================================
# Entry point
# ============================================================
def main():
    parser = argparse.ArgumentParser(
        description="Flask ant detection server for ESP32-CAM")
    parser.add_argument("--ip",    default="10.219.3.171",
                        help="ESP32-CAM IP address")
    parser.add_argument("--port",  type=int, default=80,
                        help="ESP32-CAM HTTP port")
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL,
                        help="Path to .pt model file")
    parser.add_argument("--conf",  type=float, default=DEFAULT_CONF,
                        help="Detection confidence threshold")
    parser.add_argument("--flask-port", type=int, default=FLASK_PORT,
                        help="Port for the Flask server (default: 5000)")
    args = parser.parse_args()

    if not args.model.exists():
        sys.exit(f"[ERR] Model not found: {args.model}")

    print(f"[INF] Loading model: {args.model}")
    model = YOLO(str(args.model))

    app.config['ESP_IP'] = args.ip
    app.config['CONF']   = args.conf

    # Start detection loop in background thread
    t = threading.Thread(
        target=detection_loop,
        args=(args.ip, args.port, model, args.conf),
        daemon=True
    )
    t.start()

    # Get local IP for display
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except Exception:
        local_ip = "localhost"

    print(f"\n[SRV] Flask server running:")
    print(f"[SRV] Local  : http://localhost:{args.flask_port}")
    print(f"[SRV] Network: http://{local_ip}:{args.flask_port}  (buka dari HP)")
    print(f"[SRV] Ctrl+C untuk berhenti\n")

    app.run(host='0.0.0.0', port=args.flask_port,
            debug=False, threaded=True, use_reloader=False)


if __name__ == "__main__":
    main()
