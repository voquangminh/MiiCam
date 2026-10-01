"""Base class + registry for AI detectors.

Every detector returns a list of Node objects. The aggregator draws them.
Detectors are registered by name and enabled/disabled from config.yaml.
"""

import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import numpy as np


@dataclass
class Node:
    """One detected thing, drawn as a box + label by the HUD renderer."""

    label: str
    x1: int
    y1: int
    x2: int
    y2: int
    conf: float = 0.0
    color: Tuple[int, int, int] = (0, 255, 0)
    track_id: Optional[int] = None
    # Extra keypoints to draw, as (x, y, ok) triples.
    keypoints: List[Tuple[int, int, float]] = field(default_factory=list)
    # "info" / "warn" / "alert" drives HUD badge colour.
    level: str = "info"


class Detector:
    """Base detector.

    Subclasses set `name`, `default_model` and implement `detect(frame)`.

    Lifecycle notes:
      - `load()` is called once, lazily, only when the detector is enabled.
        Keeping it lazy matters: each enabled model costs real CPU on this box.
      - `detect()` may be called less often than the video framerate (see
        `every_n_frames`). Draw code must tolerate a stale frame.
    """

    name = "base"
    default_model = "yolo11n.pt"
    default_conf = 0.4
    # Run inference every Nth frame; the aggregator holds boxes in between.
    every_n_frames = 1
    # Human label for the HUD node chip.
    display = "Base"

    def __init__(self, cfg: Optional[dict] = None):
        self.cfg = cfg or {}
        self.model_name = self.cfg.get("model", self.default_model)
        self.conf = float(self.cfg.get("conf", self.default_conf))
        self.every_n_frames = int(self.cfg.get("every_n_frames", self.every_n_frames))
        # Inference resolution. Lower = faster but misses small/far objects.
        self.imgsz = int(self.cfg.get("imgsz", 640))
        self._model = None
        self._frame_no = 0
        self._last_nodes: List[Node] = []
        self._last_run = 0.0

    # -- lifecycle ---------------------------------------------------------

    def load(self) -> None:
        """Load model weights. Override when no weights are needed."""
        from ultralytics import YOLO

        self._model = YOLO(self.model_name)

    @property
    def loaded(self) -> bool:
        return self._model is not None

    def ensure_loaded(self) -> None:
        if not self.loaded:
            self.load()

    # -- inference ---------------------------------------------------------

    def detect(self, frame: np.ndarray) -> List[Node]:
        raise NotImplementedError

    def step(self, frame: np.ndarray) -> List[Node]:
        """Called once per video frame by the aggregator.

        Skips inference on frames where this detector is off-cycle, and returns
        the previous result so overlays stay stable between inferences.
        """
        self._frame_no += 1
        if (self._frame_no - 1) % max(1, self.every_n_frames) != 0:
            return self._last_nodes
        self.ensure_loaded()
        try:
            nodes = self.detect(frame) or []
        except Exception as exc:  # never let one detector kill the stream
            self.log_error(exc)
            nodes = []
        self._last_nodes = nodes
        self._last_run = time.time()
        return nodes

    def log_error(self, exc: Exception) -> None:
        import sys

        print(f"[detector:{self.name}] ERROR {exc!r}", file=sys.stderr)

    # -- stats -------------------------------------------------------------

    @property
    def last_run_age(self) -> float:
        return 0.0 if not self._last_run else time.time() - self._last_run

    def status(self) -> dict:
        return {
            "name": self.name,
            "display": self.display,
            "loaded": self.loaded,
            "model": self.model_name,
            "conf": self.conf,
            "every_n_frames": self.every_n_frames,
            "nodes": len(self._last_nodes),
            "last_run": self._last_run,
        }


REGISTRY = {}


def register(cls):
    """Class decorator: make a detector available to the aggregator."""
    REGISTRY[cls.name] = cls
    return cls


def build(name: str, cfg: Optional[dict] = None) -> Optional[Detector]:
    """Instantiate a detector by registry name. Returns None if unknown."""
    cls = REGISTRY.get(name)
    if cls is None:
        return None
    return cls(cfg or {})


def available() -> List[str]:
    return sorted(REGISTRY.keys())