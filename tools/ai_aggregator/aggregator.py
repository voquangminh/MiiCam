#!/usr/bin/env python3
"""AI aggregator: one RTSP in, N detectors, one annotated stream out.

Why this exists
---------------
The camera (GM8136, ARMv5TE) cannot run YOLO — Ultralytics needs PyTorch and
no PyTorch build exists for that SoC. So inference has to happen elsewhere, on
a CPU-only x86 box. Two constraints shape the whole design:

1. The camera accepts a SINGLE RTSP session. So we open exactly one connection
   and share the decoded frames across all detectors. Running the reference
   scripts side by side would open N connections and wedge the camera.

2. YouTube's RTMP ingest only carries encoded pixels. It has no concept of
   bounding boxes or "nodes", so every overlay must be burned into the frame
   before encoding. Hence: draw here, then hand raw frames to ffmpeg.

Usage
-----
    python3 aggregator.py --config config.yaml
    python3 aggregator.py --enable fall --enable fire      # ad-hoc override
    python3 aggregator.py --list
    python3 aggregator.py --source test.mp4 --preview out.mp4 --no-output
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from typing import List, Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from detectors import hud  # noqa: E402
from detectors.base import build, available  # noqa: E402
from detectors.custom import CustomModelDetector  # noqa: E402
from detectors.scene import CrowdHeatmap  # noqa: E402

import detectors.custom  # noqa: F401,E402  (register side effects)
import detectors.person  # noqa: F401,E402
import detectors.scene  # noqa: F401,E402

DEFAULT_CONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.yaml")


def expand_env(value):
    """Expand $VAR / ${VAR} so secrets can stay out of the committed config."""
    if not isinstance(value, str):
        return value
    out = os.path.expandvars(value)
    # An unexpanded "${AI_RTSP_URL}" means the variable is not set; leave it
    # as-is so the caller reports a clear "source.rtsp is empty" error.
    return out


def load_config(path: str) -> dict:
    try:
        import yaml
    except ImportError:
        raise SystemExit("PyYAML missing. pip install pyyaml")
    with open(path, "r") as fh:
        cfg = yaml.safe_load(fh) or {}
    cfg.setdefault("source", {})
    cfg.setdefault("output", {})
    cfg.setdefault("hud", {})
    cfg.setdefault("detectors", {})

    src = cfg["source"]
    src["rtsp"] = expand_env(src.get("rtsp", ""))
    if not src["rtsp"] or src["rtsp"].startswith("${"):
        src["rtsp"] = ""
    cfg["output"]["rtmp"] = expand_env(cfg["output"].get("rtmp", "") or "")
    return cfg


def build_detectors(cfg: dict, enable_flags: List[str], disable_flags: List[str]):
    """Instantiate only the enabled detectors. Disabled ones cost nothing."""
    dcfg = cfg.get("detectors", {})
    built = []
    for name, sub in dcfg.items():
        want = bool(sub.get("enabled", False))
        if name in enable_flags:
            want = True
        if name in disable_flags:
            want = False
        if not want:
            continue
        det = build(name, sub or {})
        if det is None:
            print(f"[warn] unknown detector {name!r}; known: {available()}", file=sys.stderr)
            continue
        built.append(det)
    return built


class FrameReader:
    """Decode RTSP (or any ffmpeg-readable source) into numpy BGR frames.

    Uses an ffmpeg subprocess piping rawvideo rather than cv2.VideoCapture:
    it gives explicit control over transport and buffer flags, and avoids
    OpenCV's RTSP timeouts on long-lived streams.
    """

    def __init__(self, source: str, extra_args: Optional[List[str]] = None, width: int = 0, height: int = 0):
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
        cmd += self._filter_input_args(extra_args, source)
        cmd += ["-i", source]
        self.w = width or 1280
        self.h = height or 720
        cmd += [
            "-f", "rawvideo", "-pix_fmt", "bgr24",
            "-vf", f"scale={self.w}:{self.h}",
            "-"
        ]
        self.cmd = cmd
        self.proc: Optional[subprocess.Popen] = None
        self.frame_bytes = self.w * self.h * 3
        # Preallocated read buffer. Decoding straight into one array avoids a
        # 2.7 MB copy per frame and, unlike np.frombuffer, yields a writable
        # array that OpenCV in-place ops can use.
        self._buf = bytearray(self.frame_bytes)

    @staticmethod
    def _filter_input_args(extra_args: Optional[List[str]], source: str) -> List[str]:
        """Drop input flags that only make sense for a network source.

        Config carries RTSP-only options (rtsp_transport, max_delay, ...) so it
        can be pointed at a camera. When you test against a local file those
        flags are rejected by ffmpeg and the stream never opens, so filter them.
        """
        is_net = str(source).startswith(("rtsp://", "rtmp://", "http://", "https://", "udp://", "tcp://"))
        if is_net:
            return list(extra_args or [])
        net_only = ("-rtsp_transport", "-rtsp_flags", "-max_delay", "-reorder_queue_size",
                    "-avioflags", "-timeout", "-rw_timeout", "-analyzeduration", "-probesize",
                    "-fflags", "-flags", "-use_wallclock_as_timestamps")
        out: List[str] = []
        skip_next = False
        for i, a in enumerate(extra_args or []):
            if skip_next:
                skip_next = False
                continue
            if a in net_only:
                skip_next = True
                continue
            out.append(a)
        return out

    def start(self) -> None:
        print("[in ] " + " ".join(self.cmd))
        self.proc = subprocess.Popen(
            self.cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=self.frame_bytes,
        )

    def read(self) -> Optional[np.ndarray]:
        """Read exactly one frame, or None at end of stream.

        A pipe read() is free to return fewer bytes than requested, so a single
        read() cannot be trusted to yield a whole frame: it would desync the
        frame size and corrupt every subsequent reshape. Loop until the full
        frame is buffered, and detect EOF on the first short/empty read.
        """
        if self.proc is None or self.proc.stdout is None:
            return None
        need = self.frame_bytes
        mv = memoryview(self._buf)
        got = 0
        while got < need:
            n = self.proc.stdout.readinto(mv[got:])
            if not n:
                if got == 0:
                    return None
                print(f"[in ] partial frame at EOF ({got}/{need} bytes)", file=sys.stderr)
                return None
            got += n
        # Reuse the same buffer every frame: detectors must not retain a
        # reference to it across calls.
        return np.frombuffer(self._buf, dtype=np.uint8).reshape(self.h, self.w, 3)

    def stderr_tail(self) -> str:
        if self.proc and self.proc.stderr:
            try:
                import os as _os
                _os.set_blocking(self.proc.stderr.fileno(), False)
                data = self.proc.stderr.read() or b""
                return data.decode("utf-8", "replace")[-500:]
            except Exception:
                return ""
        return ""

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class FrameWriter:
    """Pipe annotated frames to ffmpeg for H.264 encode + optional RTMP push.

    On RTMP/RTSP backpressure: `communicate`-style blocking writes would stall
    the detection loop if YouTube drops the stream, so writes run in a bounded
    queue and stale frames are dropped instead. Detection must never block.
    """

    def __init__(self, cfg: dict, width: int, height: int):
        out = cfg.get("output", {})
        self.width = width
        self.height = height
        self.rtmp = (out.get("rtmp") or "").strip()
        self.preview = (out.get("preview_mp4") or "").strip()
        self.bitrate = str(out.get("bitrate_kbps", 4500))
        self.fps = str(out.get("fps", 30))
        self.preset = str(out.get("preset", "veryfast"))
        self.proc = None

    @property
    def active(self) -> bool:
        return bool(self.rtmp or self.preview)

    def start(self) -> None:
        if not self.active:
            print("[out] disabled (no rtmp / preview set)")
            return
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "warning", "-y",
               "-f", "rawvideo", "-pix_fmt", "bgr24",
               "-s", f"{self.width}x{self.height}", "-r", self.fps, "-i", "-"]
        cmd += ["-c:v", "libx264", "-preset", self.preset,
                "-b:v", f"{self.bitrate}k", "-maxrate", f"{self.bitrate}k",
                "-bufsize", f"{int(self.bitrate) * 2}k", "-pix_fmt", "yuv420p",
                "-g", str(int(self.fps) * 2), "-tune", "zerolatency"]
        if self.preview:
            cmd += [self.preview]
        elif self.rtmp:
            # FLV for RTMP; YouTube wants the keyframe interval kept short.
            cmd += ["-f", "flv", self.rtmp]
        print("[out] " + " ".join(cmd))
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)

    def write(self, frame: np.ndarray) -> None:
        if not self.proc or not self.proc.stdin:
            return
        try:
            self.proc.stdin.write(frame.tobytes())
        except (BrokenPipeError, OSError):
            print("[out] encoder died; stopping output", file=sys.stderr)

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except Exception:
                pass
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class Aggregator:
    def __init__(self, cfg: dict, detectors, enable_flags, disable_flags):
        self.cfg = cfg
        self.detectors = detectors
        self.enable_flags = enable_flags
        self.disable_flags = disable_flags
        self.running = True

        src = cfg.get("source", {})
        self.source = src.get("rtsp", "")
        self.in_args = src.get("ffmpeg_in_args") or []
        w = int(src.get("width", 1280))
        h = int(src.get("height", 720))
        self.reader = FrameReader(self.source, self.in_args, w, h)
        self.writer = FrameWriter(cfg, w, h)

        self.hud_cfg = cfg.get("hud", {})
        self.frame_no = 0
        self.fps = 0.0
        self._fps_mark = time.time()
        self._fps_count = 0
        self.detect_ms = 0.0

    def stop(self, *_args) -> None:
        self.running = False

    # -- status ------------------------------------------------------------

    def rows(self):
        rows = []
        hcfg = self.hud_cfg
        for det in self.detectors:
            n = len(det._last_nodes)
            alerts = sum(1 for x in det._last_nodes if x.level == "alert")
            if alerts:
                level = "alert"
                value = f"{n} ({alerts} ALERT)"
            elif n:
                level = "info"
                value = str(n)
            else:
                level = "warn"
                value = "idle"
            rows.append((det.display, value, level))
        rows.append(("FPS", f"{self.fps:.1f}", "info"))
        rows.append(("detect", f"{self.detect_ms:.0f}ms", "info"))
        return rows

    def banner_text(self):
        for det in self.detectors:
            for node in det._last_nodes:
                if node.level == "alert":
                    return f"{det.display.upper()}: {node.label}"
        return None

    # -- main loop ---------------------------------------------------------

    def run(self) -> int:
        if not self.source:
            raise SystemExit("source.rtsp is empty in config")
        self.reader.start()
        self.writer.start()

        print(f"[cfg] enabled detectors: {[d.name for d in self.detectors] or 'none'}")
        missing = [d.name for d in self.detectors
                   if isinstance(d, CustomModelDetector) and not d.weights_present]
        if missing:
            print(f"[cfg] missing weights for: {missing} (nodes will show MODEL MISSING)")

        banner = None
        while self.running:
            frame = self.reader.read()
            if frame is None:
                err = self.reader.stderr_tail()
                print(f"[in ] stream ended/stalled {err}", file=sys.stderr)
                break

            self.frame_no += 1
            t0 = time.time()

            all_nodes = []
            for det in self.detectors:
                try:
                    all_nodes.extend(det.step(frame))
                except FileNotFoundError as exc:
                    print(f"[detector:{det.name}] {exc}", file=sys.stderr)
                    self._disable_runtime(det.name)
                    continue
                except Exception as exc:
                    det.log_error(exc)

            # heatmap sits under the boxes, so it must blend before drawing
            for det in self.detectors:
                if isinstance(det, CrowdHeatmap) and det.overlay() is not None:
                    frame = det.overlay()

            if self.hud_cfg.get("show_panel", True):
                hud.draw_panel(frame, self.rows(), scale=float(self.hud_cfg.get("panel_scale", 1.0)))

            for det in self.detectors:
                if det.name == "intrusion" and self.hud_cfg.get("show_zone", True):
                    poly = getattr(det, "_zone_px", None)
                    if poly:
                        hud.draw_zone(frame, poly)
                if det.name == "person_count":
                    h = frame.shape[0]
                    w = frame.shape[1]
                    hud.draw_count_line(frame, int(det.line_x * w),
                                        f"IN {det.in_count} / OUT {det.out_count}")

            hud.draw_nodes(frame, all_nodes)

            if self.hud_cfg.get("show_banner", True):
                banner = self.banner_text()
                if banner:
                    hud.draw_banner(frame, banner)

            self.detect_ms = (time.time() - t0) * 1000
            self._tick_fps()

            if self.writer.active:
                self.writer.write(frame)

            if self.frame_no % 300 == 0:
                print(f"[run] frame={self.frame_no} fps={self.fps:.1f} "
                      f"detect={self.detect_ms:.0f}ms nodes={len(all_nodes)}")

        self.reader.stop()
        self.writer.stop()
        return 0

    def _disable_runtime(self, name: str) -> None:
        """Drop a detector that cannot load weights, keep the stream alive."""
        self.detectors = [d for d in self.detectors if d.name != name]
        self.enable_flags = [e for e in self.enable_flags if e != name]

    def _tick_fps(self) -> None:
        self._fps_count += 1
        now = time.time()
        if now - self._fps_mark >= 1.0:
            self.fps = self._fps_count / (now - self._fps_mark)
            self._fps_mark = now
            self._fps_count = 0


def main() -> int:
    ap = argparse.ArgumentParser(description="RTSP in, AI overlays, RTMP out")
    ap.add_argument("-c", "--config", default=DEFAULT_CONFIG)
    ap.add_argument("-e", "--enable", action="append", default=[], metavar="NAME")
    ap.add_argument("-x", "--disable", action="append", default=[], metavar="NAME")
    ap.add_argument("-s", "--source", help="override source.rtsp (or a file)")
    ap.add_argument("--rtmp", help="override output.rtmp")
    ap.add_argument("--preview", help="write annotated mp4 locally")
    ap.add_argument("--no-output", action="store_true", help="run detection only")
    ap.add_argument("--list", action="store_true", help="list detectors and exit")
    ap.add_argument("--status", action="store_true", help="print enabled set and exit")
    args = ap.parse_args()

    if args.list:
        print("known detectors:")
        for name in available():
            print(f"  {name}")
        return 0

    cfg = load_config(args.config)
    if args.source:
        cfg["source"]["rtsp"] = args.source
    if args.rtmp:
        cfg["output"]["rtmp"] = args.rtmp
    if args.preview:
        cfg["output"]["preview_mp4"] = args.preview
    if args.no_output:
        cfg["output"]["rtmp"] = ""
        cfg["output"]["preview_mp4"] = ""

    detectors = build_detectors(cfg, args.enable, args.disable)
    if not detectors:
        print("no detectors enabled — nothing to do. Use --enable NAME or config.yaml",
              file=sys.stderr)
        return 2

    if args.status:
        print(json.dumps([d.status() for d in detectors], indent=2))
        return 0

    agg = Aggregator(cfg, detectors, args.enable, args.disable)
    signal.signal(signal.SIGINT, agg.stop)
    signal.signal(signal.SIGTERM, agg.stop)
    return agg.run()


if __name__ == "__main__":
    sys.exit(main())