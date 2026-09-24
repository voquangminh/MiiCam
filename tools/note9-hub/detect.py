#!/data/data/com.termux/files/usr/bin/python3
import json
import os
import sys
import time

W = int(os.environ.get("AI_WIDTH", "640"))
H = int(os.environ.get("AI_HEIGHT", "360"))
FRAME_BYTES = W * H * 3
MOTION_TH = int(os.environ.get("AI_MOTION_THRESHOLD", "25"))
MIN_AREA = int(os.environ.get("AI_MIN_AREA", "1500"))
COOLDOWN = float(os.environ.get("AI_EVENT_COOLDOWN", "10"))
SNAP_DIR = os.environ.get(
    "AI_SNAPSHOT_DIR", os.path.expanduser("~/.note9-hub/snapshots")
)
MODEL_PATH = os.environ.get("AI_TFLITE_MODEL", "")
LABELS_PATH = os.environ.get("AI_LABELS", "")
CLASSES = [
    c.strip()
    for c in os.environ.get(
        "AI_CLASSES", "person,car,truck,bus,motorcycle,bicycle"
    ).split(",")
    if c.strip()
]
SCORE_TH = float(os.environ.get("AI_SCORE_THRESHOLD", "0.40"))

try:
    import numpy as np
except ImportError:
    print("numpy missing: pip install numpy", file=sys.stderr)
    sys.exit(1)

try:
    from PIL import Image, ImageDraw

    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

interp = None
labels = []


def load_labels(path):
    if not path or not os.path.isfile(path):
        return []
    with open(path) as f:
        return [line.strip() for line in f if line.strip()]


def load_tflite(path):
    if not path or not os.path.isfile(path):
        return None
    if not HAVE_PIL:
        print("[ai] model set but Pillow missing, motion-only", file=sys.stderr)
        return None
    try:
        from tflite_runtime.interpreter import Interpreter

        it = Interpreter(model_path=path)
        it.allocate_tensors()
        return it
    except Exception as e:
        print(f"[ai] tflite load failed: {e}, motion-only", file=sys.stderr)
        return None


def detect(it, arr):
    det_in = it.get_input_details()[0]
    ih, iw = det_in["shape"][1], det_in["shape"][2]
    dtype = det_in["dtype"]
    img = Image.fromarray(arr).resize((iw, ih))
    if np.issubdtype(dtype, np.floating):
        data = np.expand_dims(np.asarray(img, dtype=np.float32) / 255.0, 0)
    else:
        data = np.expand_dims(np.asarray(img, dtype=dtype), 0)
    it.set_tensor(det_in["index"], data)
    it.invoke()

    outs = {o["name"]: it.get_tensor(o["index"]) for o in it.get_output_details()}
    boxes = next(v for v in outs.values() if v.ndim == 3 and v.shape[-1] == 4)[0]
    scores = None
    classes = None
    for name, v in outs.items():
        if v.ndim == 2 and v.shape[-1] == 1:
            continue
        if v.ndim == 2 and v.shape[-1] > 1 and scores is None:
            scores = v[0]
        elif v.ndim == 2 and scores is not None:
            classes = v[0].astype(int)
    if scores is None or classes is None:
        return []

    results = []
    for i, s in enumerate(scores):
        if s < SCORE_TH:
            continue
        name = labels[classes[i]] if classes[i] < len(labels) else str(classes[i])
        if labels and CLASSES and name not in CLASSES:
            continue
        y1, x1, y2, x2 = boxes[i]
        results.append(
            {
                "label": name,
                "score": round(float(s), 3),
                "box": [
                    int(x1 * W),
                    int(y1 * H),
                    int(x2 * W),
                    int(y2 * H),
                ],
            }
        )
    return results


def save_snapshot(arr, boxes, tag):
    if not HAVE_PIL:
        return None
    ts = time.strftime("%Y%m%d_%H%M%S")
    path = os.path.join(SNAP_DIR, f"{tag}_{ts}.jpg")
    img = Image.fromarray(arr)
    if boxes:
        d = ImageDraw.Draw(img)
        for b in boxes:
            d.rectangle(b["box"], outline=(255, 0, 0), width=2)
            d.text((b["box"][0] + 2, b["box"][1] + 2), b["label"], fill=(255, 0, 0))
    img.save(path, quality=85)
    return path


def main():
    global interp, labels
    labels = load_labels(LABELS_PATH)
    interp = load_tflite(MODEL_PATH)
    stdin = sys.stdin.buffer
    prev = None
    last_event = 0.0
    last_stats = time.time()
    frames = 0

    while True:
        buf = stdin.read(FRAME_BYTES)
        if not buf or len(buf) < FRAME_BYTES:
            break
        arr = np.frombuffer(buf, np.uint8).reshape(H, W, 3)
        gray = arr.mean(axis=2)
        motion_area = 0
        if prev is not None:
            diff = np.abs(gray.astype(np.int16) - prev.astype(np.int16))
            motion_area = int((diff > MOTION_TH).sum())
        prev = gray

        boxes = detect(interp, arr) if interp else []
        now = time.time()
        if (motion_area > MIN_AREA or boxes) and now - last_event >= COOLDOWN:
            last_event = now
            snap = save_snapshot(
                arr, boxes, "det" if boxes else "motion"
            )
            print(
                json.dumps(
                    {
                        "ts": int(now),
                        "motion_area": motion_area,
                        "boxes": boxes,
                        "snapshot": snap,
                    }
                ),
                flush=True,
            )

        frames += 1
        if now - last_stats >= 60:
            fps = frames / max(now - last_stats, 1)
            mode = "tflite+motion" if interp else "motion-only"
            print(f"[ai] {fps:.1f} fps, {mode}", file=sys.stderr, flush=True)
            last_stats = now
            frames = 0


if __name__ == "__main__":
    main()
