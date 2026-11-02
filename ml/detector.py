"""Python twin of the firmware's posterior smoothing (kws_detector.c).

Used to estimate false accepts on a continuous stream. Keep both in sync.
"""
from collections import deque

import numpy as np

import kws_config as cfg


class Detector:
    def __init__(self, n_labels, threshold=cfg.DETECTOR_DEFAULTS["threshold"],
                 window_ms=cfg.DETECTOR_DEFAULTS["window_ms"], min_results=cfg.DETECTOR_DEFAULTS["min_results"],
                 suppression_ms=cfg.DETECTOR_DEFAULTS["suppression_ms"], first_keyword=2):
        self.n = n_labels
        self.threshold = threshold
        self.window_ms = window_ms
        self.min_results = min_results
        self.suppression_ms = suppression_ms
        self.first_keyword = first_keyword
        self.history = deque()
        self.last_detection_ms = None

    def update(self, probs, now_ms):
        """Feed one probability vector; return (label_index, score) or None."""
        self.history.append((now_ms, np.asarray(probs, dtype=np.float32)))
        while now_ms - self.history[0][0] > self.window_ms:
            self.history.popleft()
        if len(self.history) < self.min_results:
            return None
        avg = np.mean([p for _, p in self.history], axis=0)
        best = int(np.argmax(avg))
        if best < self.first_keyword or avg[best] < self.threshold:
            return None
        if self.last_detection_ms is not None and now_ms - self.last_detection_ms < self.suppression_ms:
            return None
        self.last_detection_ms = now_ms
        return best, float(avg[best])
