"""Custom-model detectors.

These need weights that are NOT shipped with the aggregator: each reference
project expects a `best.pt` trained in Roboflow. The aggregator degrades
gracefully when weights are missing — the node shows "MODEL MISSING" instead of
killing the stream, so you can drop weights in later and toggle it on.
"""

import os
import re
from typing import List

import numpy as np

from .base import Detector, Node, register

WEIGHTS_DIR = os.environ.get("AI_WEIGHTS_DIR", "./weights")

# Stock Ultralytics release weights: Ultralytics downloads these on demand.
STOCK_MODEL_RE = re.compile(r"^(yolo|yolov|ultralytics)[\w.-]*\.pt$", re.I)


class CustomModelDetector(Detector):
    """Base for detectors whose weights must be supplied by the user."""

    default_model = "best.pt"

    def __init__(self, cfg=None):
        super().__init__(cfg)
        self.model_name = self._resolve(self.model_name)

    @staticmethod
    def _resolve(name: str) -> str:
        """Resolve a bare filename inside the weights dir.

        Subclasses that need stock Ultralytics weights (e.g. vehicle, which uses
        yolo11n.pt out of the box) must not be flagged as "missing weights", so
        they report weights_present=True whenever Ultralytics can auto-download.
        """
        if os.path.isabs(name) or os.path.exists(name):
            return name
        candidate = os.path.join(WEIGHTS_DIR, name)
        if os.path.exists(candidate):
            return candidate
        return name

    @property
    def weights_present(self) -> bool:
        """True when weights are on disk, or Ultralytics can fetch them itself.

        Stock `.pt` names (yolo11n.pt, yolo11n-pose.pt, ...) are auto-downloaded
        by Ultralytics on first use, so they count as present even before they
        exist locally. Custom Roboflow `best.pt` exports must be supplied.
        """
        return os.path.exists(self.model_name) or STOCK_MODEL_RE.match(self.model_name) is not None

    def ensure_loaded(self) -> None:
        if self.loaded:
            return
        if not self.weights_present:
            raise FileNotFoundError(
                f"{self.name}: weights not found at {self.model_name!r}. "
                f"Put the file in {WEIGHTS_DIR}/ or set model: in config.yaml."
            )
        from ultralytics import YOLO

        self._model = YOLO(self.model_name)

    def status(self) -> dict:
        st = super().status()
        st["weights_present"] = self.weights_present
        st["model_path"] = self.model_name
        return st


@register
class FireDetector(CustomModelDetector):
    """Fire + smoke. Needs a Roboflow-trained best.pt (classes: fire, smoke)."""

    name = "fire"
    display = "Fire / Smoke"
    default_model = "fire.pt"
    conf = 0.4

    def detect(self, frame: np.ndarray) -> List[Node]:
        results = self._model(frame, conf=self.conf, imgsz=self.imgsz, verbose=False)[0]
        nodes: List[Node] = []
        names = getattr(self._model, "names", {}) or {}
        for i, box in enumerate(results.boxes):
            x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
            cf = float(box.conf[0])
            ci = int(box.cls[0])
            cls_name = names.get(ci, str(ci))
            alert = "fire" in cls_name.lower()
            nodes.append(Node(
                label=f"{cls_name} {cf:.0%}",
                x1=x1, y1=y1, x2=x2, y2=y2, conf=cf,
                color=(0, 0, 255) if alert else (0, 165, 255),
                level="alert" if alert else "warn",
            ))
        return nodes


@register
class PPEDetector(CustomModelDetector):
    """Helmet / vest compliance. Needs a Roboflow-trained best.pt."""

    name = "ppe"
    display = "PPE Compliance"
    default_model = "ppe.pt"
    conf = 0.4

    def detect(self, frame: np.ndarray) -> List[Node]:
        results = self._model(frame, conf=self.conf, imgsz=self.imgsz, verbose=False)[0]
        nodes: List[Node] = []
        names = getattr(self._model, "names", {}) or {}
        for box in results.boxes:
            x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
            cf = float(box.conf[0])
            cls_name = names.get(int(box.cls[0]), str(int(box.cls[0])))
            missing = "no" in cls_name.lower() or "missing" in cls_name.lower()
            nodes.append(Node(
                label=cls_name,
                x1=x1, y1=y1, x2=x2, y2=y2, conf=cf,
                color=(0, 0, 255) if missing else (0, 200, 0),
                level="alert" if missing else "info",
            ))
        return nodes


@register
class RatDetector(CustomModelDetector):
    """Rodent / pest detection. Needs a Roboflow-trained best.pt."""

    name = "rat"
    display = "Rodent"
    default_model = "rat.pt"
    conf = 0.4

    def detect(self, frame: np.ndarray) -> List[Node]:
        results = self._model(frame, conf=self.conf, imgsz=self.imgsz, verbose=False)[0]
        nodes: List[Node] = []
        for box in results.boxes:
            x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
            nodes.append(Node(
                label=f"RODENT {float(box.conf[0]):.0%}",
                x1=x1, y1=y1, x2=x2, y2=y2, conf=float(box.conf[0]),
                color=(0, 0, 255), level="alert",
            ))
        return nodes


@register
class PlateDetector(CustomModelDetector):
    """Licence plate detection + OCR-free box overlay."""

    name = "plate"
    display = "License Plate"
    default_model = "plate.pt"
    conf = 0.4

    def detect(self, frame: np.ndarray) -> List[Node]:
        results = self._model(frame, conf=self.conf, imgsz=self.imgsz, verbose=False)[0]
        nodes: List[Node] = []
        for box in results.boxes:
            x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
            nodes.append(Node(
                label=f"PLATE {float(box.conf[0]):.0%}",
                x1=x1, y1=y1, x2=x2, y2=y2, conf=float(box.conf[0]),
                color=(0, 200, 255), level="warn",
            ))
        return nodes


@register
class VehicleDetector(CustomModelDetector):
    """Vehicle detection + tracking. Falls back to stock COCO weights,
    which already know car/bus/truck/motorcycle, so it runs with no setup."""

    name = "vehicle"
    display = "Vehicles"
    default_model = "yolo11n.pt"
    conf = 0.4
    # COCO ids: 2 car, 3 bus, 5 truck, 7 motorcycle, 8 bicycle
    vehicle_classes = [2, 3, 5, 7, 8]

    def detect(self, frame: np.ndarray) -> List[Node]:
        results = self._model(
            frame, conf=self.conf, classes=self.vehicle_classes,
            imgsz=self.imgsz, verbose=False
        )[0]
        nodes: List[Node] = []
        names = getattr(self._model, "names", {}) or {}
        for box in results.boxes:
            x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
            cf = float(box.conf[0])
            cls_name = names.get(int(box.cls[0]), "vehicle")
            nodes.append(Node(
                label=f"{cls_name} {cf:.0%}",
                x1=x1, y1=y1, x2=x2, y2=y2, conf=cf,
                color=(255, 200, 0), level="info",
            ))
        return nodes