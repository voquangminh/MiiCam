"""Whole-scene detectors: crowd heatmap and basic person presence.

The heatmap port keeps a decaying float buffer, so it needs a `reset()` when
the stream restarts or the resolution changes. Logic ported from
Nawaf-Rayhan585/YOLO_Projects crowd-heatmap.
"""

from typing import List, Optional

import numpy as np

from .base import Detector, Node, register


@register
class CrowdHeatmap(Detector):
    """Live decaying heatmap of where people have been, plus a head count."""

    name = "crowd"
    display = "Crowd Heatmap"
    default_model = "yolo11n.pt"
    default_conf = 0.4

    def __init__(self, cfg: Optional[dict] = None):
        super().__init__(cfg)
        self.heatmap = None
        self.people_count = 0
        self.decay = float(self.cfg.get("decay", 0.98))
        self.radius = int(self.cfg.get("radius", 30))
        self.alpha = float(self.cfg.get("alpha", 0.3))
        self._overlay = None

    def reset(self, shape) -> None:
        self.heatmap = np.zeros(shape, dtype=np.float32)
        self._overlay = None

    def detect(self, frame: np.ndarray) -> List[Node]:
        import cv2

        h, w = frame.shape[:2]
        if self.heatmap is None or self.heatmap.shape != (h, w):
            self.reset((h, w))
        self.heatmap *= self.decay

        results = self._model(frame, classes=[0], conf=self.conf, imgsz=self.imgsz, verbose=False)[0]
        boxes = list(results.boxes)
        self.people_count = len(boxes)

        nodes: List[Node] = []
        for box in boxes:
            x1, y1, x2, y2 = map(int, box.xyxy[0])
            cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
            cv2.circle(self.heatmap, (cx, cy), self.radius, 1, -1)
            nodes.append(Node(
                label="person", x1=x1, y1=y1, x2=x2, y2=y2,
                conf=float(box.conf[0]), color=(0, 255, 0), level="info",
            ))

        heat_blur = cv2.GaussianBlur(self.heatmap, (0, 0), 15)
        heat_norm = cv2.normalize(heat_blur, None, 0, 255, cv2.NORM_MINMAX).astype(np.uint8)
        heat_color = cv2.applyColorMap(heat_norm, cv2.COLORMAP_JET)
        self._overlay = cv2.addWeighted(frame, 1.0 - self.alpha, heat_color, self.alpha, 0)
        return nodes

    def overlay(self) -> Optional[np.ndarray]:
        return self._overlay


@register
class PersonPresence(Detector):
    """Cheapest possible node: is anyone in frame. Uses stock COCO weights."""

    name = "presence"
    display = "Person Present"
    default_model = "yolo11n.pt"
    conf = 0.4
    every_n_frames = 3

    def detect(self, frame: np.ndarray) -> List[Node]:
        results = self._model(frame, classes=[0], conf=self.conf, imgsz=self.imgsz, verbose=False)[0]
        nodes: List[Node] = []
        for box in results.boxes:
            x1, y1, x2, y2 = map(int, box.xyxy[0])
            cf = float(box.conf[0])
            nodes.append(Node(
                label=f"person {cf:.0%}", x1=x1, y1=y1, x2=x2, y2=y2,
                conf=cf, color=(0, 255, 0), level="info",
            ))
        return nodes