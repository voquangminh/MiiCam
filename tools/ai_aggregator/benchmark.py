#!/usr/bin/env python3
"""Benchmark: run detectors over a clip, report per-detector inference cost.

Separates model load time from per-frame cost, and shows what a combined set
costs so you can budget which detectors fit on CPU-only hardware.

    python3 benchmark.py --source clip.mp4 --detectors presence,fall,crowd
    python3 benchmark.py --source clip.mp4 --detectors all --imgsz 640
"""

import argparse
import os
import sys
import time
from typing import List

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import detectors.custom  # noqa: F401,E402
import detectors.person  # noqa: F401,E402
import detectors.scene  # noqa: F401,E402
from detectors.base import REGISTRY, build  # noqa: E402

W, H = 1280, 720


def torch_threads() -> int:
    """Report the thread count torch will actually use, for honest numbers."""
    try:
        import torch

        return int(torch.get_num_threads())
    except Exception:
        return 0


def load_frames(source: str, width: int, height: int, limit: int) -> List[np.ndarray]:
    """Decode a clip to frames. Reuses the aggregator's ffmpeg approach."""
    from aggregator import FrameReader

    reader = FrameReader(source, [], width, height)
    reader.start()
    frames = []
    try:
        while len(frames) < limit:
            f = reader.read()
            if f is None:
                break
            frames.append(f.copy())
    finally:
        reader.stop()
    return frames


def bench_one(name: str, frames: List[np.ndarray], imgsz: int, warmup: int):
    det = build(name, {"imgsz": imgsz} if imgsz else {})
    if det is None:
        return None

    # Force load separately so load time is not mixed into inference.
    t0 = time.time()
    try:
        det.ensure_loaded()
    except Exception as exc:
        return {"name": name, "error": str(exc)}
    load_s = time.time() - t0

    n_warm = min(warmup, len(frames))
    for f in frames[:n_warm]:
        try:
            det.detect(f)
        except Exception:
            pass

    times = []
    for f in frames[n_warm:]:
        t0 = time.time()
        try:
            det.detect(f)
        except Exception as exc:
            return {"name": name, "error": f"infer: {exc!r}", "load_s": round(load_s, 2)}
        times.append((time.time() - t0) * 1000)

    if not times:
        return {"name": name, "error": "no frames", "load_s": round(load_s, 2)}
    arr = np.array(times)
    return {
        "name": name,
        "display": det.display,
        "load_s": round(load_s, 2),
        "mean_ms": round(float(arr.mean()), 1),
        "p50_ms": round(float(np.percentile(arr, 50)), 1),
        "p95_ms": round(float(np.percentile(arr, 95)), 1),
        "max_fps": round(1000.0 / float(arr.mean()), 1),
        "cycles": int(len(arr)),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--source", required=True)
    ap.add_argument("-d", "--detectors", default="all")
    ap.add_argument("--imgsz", type=int, default=640)
    ap.add_argument("--frames", type=int, default=20)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("-w", "--width", type=int, default=W)
    ap.add_argument("--height", type=int, default=H)
    args = ap.parse_args()

    names = sorted(REGISTRY.keys()) if args.detectors == "all" else \
        [x.strip() for x in args.detectors.split(",") if x.strip()]

    print(f"loading {args.frames} frames from {args.source} ...")
    t0 = time.time()
    frames = load_frames(args.source, args.width, args.height, args.frames)
    if not frames:
        print("could not decode any frames", file=sys.stderr)
        return 1
    print(f"decoded {len(frames)} frames in {time.time()-t0:.1f}s "
          f"({frames[0].shape[1]}x{frames[0].shape[0]})")
    th = torch_threads()
    if th:
        print(f"torch threads: {th} (CPU-only inference scales with this)\n")

    rows = []
    for name in names:
        print(f"benching {name} ...", flush=True)
        res = bench_one(name, frames, args.imgsz, args.warmup)
        if res:
            rows.append(res)

    print()
    hdr = f"{'detector':<14}{'load s':>8}{'mean ms':>10}{'p50':>8}{'p95':>8}{'max fps':>10}"
    print(hdr)
    print("-" * len(hdr))
    total = 0.0
    for r in rows:
        if "error" in r:
            print(f"{r['name']:<14}  ERROR: {r['error'][:44]}")
            continue
        total += r["mean_ms"]
        print(f"{r['name']:<14}{r['load_s']:>8.2f}{r['mean_ms']:>10.1f}"
              f"{r['p50_ms']:>8.1f}{r['p95_ms']:>8.1f}{r['max_fps']:>10.1f}")

    ok = [r for r in rows if "error" not in r]
    if ok:
        print()
        print(f"sum of mean per-frame cost: {total:.1f} ms "
              f"-> ceiling {1000/total:.1f} fps for all of them together")
        print(f"at 30 fps budget (33.3 ms/frame) you can afford ~{33.3/ (total/len(ok)):.1f} "
              f"such detectors on average")
        print("\nNote: every_n_frames divides the effective cost — a detector at")
        print("every_n_frames=4 contributes mean_ms/4 to a per-frame budget.")
    return 0


if __name__ == "__main__":
    sys.exit(main())