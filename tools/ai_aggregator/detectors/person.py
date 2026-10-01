"""Person-based detectors: fall, intrusion, loitering-lie (suspicious), person counter.

These share one pose/detect pipeline and differ in the rule applied to tracked
people, so they live together. Logic is ported from the reference projects in
Nawaf-Rayhan585/YOLO_Projects (fall-detection, restricted-zone-intrusion-alert,
suspicious-behavior-detection, people-counter-in-out).
"""

import time
from collections import defaultdict, deque
from typing import List, Optional

import numpy as np

from .base import Detector, Node, register

LEFT_SHOULDER, RIGHT_SHOULDER = 5, 6
LEFT_HIP, RIGHT_HIP = 11, 12


class PersonBase(Detector):
    """Shared tracking bookkeeping for person detectors."""

    default_model = "yolo11n.pt"
    conf = 0.45

    def __init__(self, cfg: Optional[dict] = None):
        super().__init__(cfg)
        self.history = defaultdict(lambda: deque(maxlen=self.window))
        self.state = {}
        self.last_alert = defaultdict(float)
        self.seen_counts = defaultdict(int)
        self.first_seen = {}
        self._prev_ids = set()

    @property
    def window(self) -> int:
        return int(self.cfg.get("window", 8))

    def track(self, frame: np.ndarray):
        """Run YOLO tracking. Returns (ids, boxes) or (None, None)."""
        results = self._model.track(
            frame, persist=True, conf=self.conf, classes=[0],
            imgsz=self.imgsz, verbose=False
        )[0]
        if results.boxes.id is None or len(results.boxes) == 0:
            return None, None
        return (
            results.boxes.id.int().tolist(),
            results.boxes.xyxy.tolist(),
            results.boxes.conf.tolist(),
        )

    def prune(self) -> None:
        """Forget tracks that vanished, so stale state cannot fire an alert."""
        gone = set(self.history.keys()) - self._prev_ids
        for tid in gone:
            self.history.pop(tid, None)
            self.state.pop(tid, None)
            self.first_seen.pop(tid, None)
            self.seen_counts.pop(tid, None)

    def maybe_alert(self, track_id: int, text: str) -> bool:
        """True when cooldown allows emitting a new alert for this track."""
        cooldown = float(self.cfg.get("cooldown", 5.0))
        now = time.time()
        if now - self.last_alert[track_id] > cooldown:
            self.last_alert[track_id] = now
            print(f"[ALERT:{self.name}] {text} (id={track_id})")
            return True
        return False


@register
class FallDetector(PersonBase):
    """Flags a person when their torso centre drops sharply and posture turns
    horizontal. Pose heuristic, no training data needed."""

    name = "fall"
    display = "Fall Detection"
    default_model = "yolo11n-pose.pt"
    conf = 0.5

    def detect(self, frame: np.ndarray) -> List[Node]:
        results = self._model.track(
            frame, persist=True, conf=self.conf, imgsz=self.imgsz, verbose=False
        )[0]
        nodes: List[Node] = []
        self._prev_ids = set()

        if results.boxes.id is None or results.keypoints is None:
            self.prune()
            return nodes

        ids = results.boxes.id.int().tolist()
        boxes = results.boxes.xyxy.tolist()
        kpts_all = results.keypoints.xy.tolist()
        self._prev_ids = set(ids)

        drop_thresh = float(self.cfg.get("drop_thresh", 0.5))
        lying_ratio = float(self.cfg.get("lying_ratio", 1.2))

        for tid, box, kpts in zip(ids, boxes, kpts_all):
            x1, y1, x2, y2 = box
            w, h = x2 - x1, y2 - y1
            if w <= 0 or h <= 0:
                continue

            sh = [(kpts[LEFT_SHOULDER][0], kpts[LEFT_SHOULDER][1]),
                  (kpts[RIGHT_SHOULDER][0], kpts[RIGHT_SHOULDER][1])]
            hp = [(kpts[LEFT_HIP][0], kpts[LEFT_HIP][1]),
                  (kpts[RIGHT_HIP][0], kpts[RIGHT_HIP][1])]

            # Fall back to bbox centre when keypoints are missing.
            valid = all(abs(p[1]) > 0 for p in sh + hp)
            if valid:
                sy = (sh[0][1] + sh[1][1]) / 2
                hy = (hp[0][1] + hp[1][1]) / 2
                center_y = (sy + hy) / 2
            else:
                center_y = (y1 + y2) / 2

            hist = self.history[tid]
            hist.append(center_y)

            lying = (w / h) > lying_ratio
            dropped = False
            if len(hist) == hist.maxlen:
                drop = (hist[-1] - hist[0]) / h
                dropped = drop > drop_thresh

            if dropped and lying:
                self.state[tid] = True
            elif not lying:
                self.state[tid] = False

            fallen = bool(self.state.get(tid, False))

            kp_out = []
            for kx, ky in kpts:
                ok = 1.0 if (abs(kx) > 0 and abs(ky) > 0) else 0.0
                kp_out.append((int(kx), int(ky), ok))

            nodes.append(Node(
                label=f"ID {tid} FALL" if fallen else f"ID {tid}",
                x1=int(x1), y1=int(y1), x2=int(x2), y2=int(y2),
                conf=0.0,
                color=(0, 0, 255) if fallen else (0, 255, 0),
                track_id=tid,
                keypoints=kp_out,
                level="alert" if fallen else "info",
            ))

            if fallen:
                self.maybe_alert(tid, "FALL DETECTED")

        self.prune()
        return nodes


@register
class IntrusionDetector(PersonBase):
    """Alerts when a tracked person enters a normalized polygon zone."""

    name = "intrusion"
    display = "Zone Intrusion"
    default_model = "yolo11n.pt"
    conf = 0.45

    def __init__(self, cfg=None):
        super().__init__(cfg)
        pts = self.cfg.get("polygon") or [[0.35, 0.35], [0.65, 0.35], [0.65, 0.65], [0.35, 0.65]]
        self.polygon = np.array(pts, dtype=np.float32)

    def detect(self, frame: np.ndarray) -> List[Node]:
        ids, boxes, confs = self.track(frame)
        nodes: List[Node] = []
        if ids is None:
            self._prev_ids = set()
            self.prune()
            return nodes
        self._prev_ids = set(ids)

        h, w = frame.shape[:2]
        poly_px = self.polygon * np.array([w, h], dtype=np.float32)
        show_zone = bool(self.cfg.get("show_zone", True))

        for tid, box, cf in zip(ids, boxes, confs):
            x1, y1, x2, y2 = box
            bw, bh = x2 - x1, y2 - y1
            if bw <= 0 or bh <= 0:
                continue

            foot = np.array([[[x1 + bw / 2, y2]]], dtype=np.float32)
            inside = cv2_point_in_poly(foot[0][0], poly_px)
            if inside:
                self.state[tid] = True
                self.maybe_alert(tid, "ZONE INTRUSION")
            level = "alert" if inside else "info"
            nodes.append(Node(
                label=f"ID {tid} IN ZONE" if inside else f"ID {tid}",
                x1=int(x1), y1=int(y1), x2=int(x2), y2=int(y2),
                conf=float(cf),
                color=(0, 0, 255) if inside else (0, 200, 255),
                track_id=tid,
                level=level,
            ))

        if show_zone:
            self._zone_px = poly_px.astype(int).tolist()

        self.prune()
        return nodes


def cv2_point_in_poly(point, poly) -> bool:
    """Ray-casting point-in-polygon, avoids importing cv2 just for this."""
    x, y = point
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-9) + xi):
            inside = not inside
        j = i
    return inside


@register
class PersonCounter(PersonBase):
    """Counts people entering and leaving across a line."""

    name = "person_count"
    display = "Person Counter"
    default_model = "yolo11n.pt"
    conf = 0.45

    def __init__(self, cfg=None):
        super().__init__(cfg)
        self.in_count = 0
        self.out_count = 0
        self.line_x = float(self.cfg.get("line", 0.5))

    def detect(self, frame: np.ndarray) -> List[Node]:
        ids, boxes, confs = self.track(frame)
        nodes: List[Node] = []
        if ids is None:
            self._prev_ids = set()
            self.prune()
            return nodes
        self._prev_ids = set(ids)

        w = frame.shape[1]
        line_px = self.line_x * w

        for tid, box, cf in zip(ids, boxes, confs):
            x1, y1, x2, y2 = box
            bw, bh = x2 - x1, y2 - y1
            if bw <= 0 or bh <= 0:
                continue
            cx = x1 + bw / 2

            prev = self.state.get(tid)
            if prev is not None:
                if prev <= line_px and cx > line_px:
                    self.in_count += 1
                    self.maybe_alert(tid, f"IN (total {self.in_count})")
                elif prev >= line_px and cx < line_px:
                    self.out_count += 1
                    self.maybe_alert(tid, f"OUT (total {self.out_count})")
            self.state[tid] = cx

            nodes.append(Node(
                label=f"ID {tid}",
                x1=int(x1), y1=int(y1), x2=int(x2), y2=int(y2),
                conf=float(cf), color=(0, 255, 0), track_id=tid,
            ))

        self.prune()
        return nodes