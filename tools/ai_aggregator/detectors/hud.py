"""HUD renderer: draws detector nodes + a status panel onto frames.

Design constraint: detection is slower than the video framerate on CPU-only
hardware, so the HUD must be cheap. It draws only what the detectors last
returned; the aggregator keeps those results between inference frames. That is
why box drawing lives here and is decoupled from inference.
"""

from typing import Dict, List, Optional, Tuple

import numpy as np

FONT = 0  # cv2.FONT_HERSHEY_SIMPLEX

LEVEL_COLORS = {
    "info": (0, 255, 0),
    "warn": (0, 165, 255),
    "alert": (0, 0, 255),
}

SKELETON = [
    (5, 7), (7, 9), (6, 8), (8, 10), (5, 6), (5, 11), (6, 12),
    (11, 12), (11, 13), (12, 14), (13, 15), (14, 16),
]


def draw_nodes(frame: np.ndarray, nodes: List, thickness: int = 2) -> None:
    """Draw every node box, its label, and pose keypoints if present."""
    import cv2

    for node in nodes:
        color = node.color or LEVEL_COLORS.get(node.level, (0, 255, 0))

        if node.keypoints:
            pts = node.keypoints
            for a, b in SKELETON:
                if a >= len(pts) or b >= len(pts):
                    continue
                ax, ay, aok = pts[a]
                bx, by, bok = pts[b]
                if aok > 0 and bok > 0:
                    cv2.line(frame, (ax, ay), (bx, by), color, thickness)
            for x, y, ok in pts:
                if ok > 0:
                    cv2.circle(frame, (x, y), 3, color, -1)

        cv2.rectangle(frame, (node.x1, node.y1), (node.x2, node.y2), color, thickness)

        label = node.label
        (tw, th), base = cv2.getTextSize(label, FONT, 0.55, 2)
        ty = node.y1 - 8 if node.y1 - 8 - th > 0 else node.y1 + th + 8
        cv2.rectangle(frame, (node.x1, ty - th - base), (node.x1 + tw + 6, ty + 4), color, -1)
        cv2.putText(frame, label, (node.x1 + 3, ty - 2), FONT, 0.55, (0, 0, 0), 2, cv2.LINE_AA)


def draw_panel(
    frame: np.ndarray,
    rows: List[Tuple[str, str, str]],
    origin: Tuple[int, int] = (14, 14),
    scale: float = 0.52,
) -> None:
    """Top-left status panel.

    `rows` is a list of (name, value, level). Empty rows are skipped, so the
    panel only grows with the number of enabled detectors.
    """
    import cv2

    if not rows:
        return

    line_h = 22
    pad = 10
    box_h = pad * 2 + line_h * len(rows)
    x0, y0 = origin
    box_w = 250

    overlay = frame.copy()
    import cv2 as _cv2
    _cv2.rectangle(overlay, (x0, y0), (x0 + box_w, y0 + box_h), (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.55, frame, 0.45, 0, frame)
    cv2.rectangle(frame, (x0, y0), (x0 + box_w, y0 + box_h), (70, 70, 70), 1)

    for i, (name, value, level) in enumerate(rows):
        color = LEVEL_COLORS.get(level, (230, 230, 230))
        y = y0 + pad + line_h * i + line_h - 8
        cv2.putText(frame, name, (x0 + pad, y), FONT, scale, (170, 170, 170), 1, cv2.LINE_AA)
        cv2.putText(frame, value, (x0 + pad + 92, y), FONT, scale, color, 1, cv2.LINE_AA)


def draw_zone(frame: np.ndarray, polygon_pts, color=(0, 200, 255)) -> None:
    """Outline a normalized intrusion polygon given in pixel coords."""
    import cv2

    pts = np.array(polygon_pts, dtype=np.int32).reshape(-1, 1, 2)
    cv2.polylines(frame, [pts], True, color, 2)
    cv2.putText(frame, "ZONE", (pts[0][0][0] + 4, pts[0][0][1] - 8), FONT, 0.5, color, 1, cv2.LINE_AA)


def draw_count_line(frame: np.ndarray, x_px: int, label: str) -> None:
    import cv2

    h = frame.shape[0]
    cv2.line(frame, (x_px, 0), (x_px, h), (255, 255, 0), 2)
    cv2.putText(frame, label, (x_px + 6, 90), FONT, 0.5, (255, 255, 0), 1, cv2.LINE_AA)


def draw_banner(frame: np.ndarray, text: str, color=(0, 0, 255)) -> None:
    """Big centred banner for alert-level events."""
    import cv2

    h, w = frame.shape[:2]
    (tw, th), _ = cv2.getTextSize(text, FONT, 1.3, 3)
    x = max(10, (w - tw) // 2)
    cv2.rectangle(frame, (x - 12, 10), (x + tw + 12, 10 + th + 18), (0, 0, 0), -1)
    cv2.putText(frame, text, (x, 10 + th + 6), FONT, 1.3, color, 3, cv2.LINE_AA)