"""Feature extraction using the same int8 audio preprocessor model the firmware runs.

Running the identical .tflite graph through the TFLM Python runtime keeps training
features bit-exact with the device (same kernels, same fixed-point arithmetic).
"""
import numpy as np
from tflite_micro.python.tflite_micro import runtime

import kws_config as cfg


class FeatureExtractor:
    def __init__(self, model_path=cfg.PREPROCESSOR_MODEL):
        self._interp = runtime.Interpreter.from_bytes(model_path.read_bytes())
        self._in_shape = self._interp.get_input_details(0)["shape"]

    def frame(self, window: np.ndarray) -> np.ndarray:
        """One 30 ms int16 window -> 40 int8 features. Stateful (noise estimate)."""
        self._interp.set_input(window.reshape(self._in_shape).astype(np.int16), 0)
        self._interp.invoke()
        return self._interp.get_output(0).reshape(cfg.FEATURE_CHANNELS).copy()

    def clip(self, audio: np.ndarray, warmup: np.ndarray = None) -> np.ndarray:
        """1 s int16 clip -> (49, 40) int8 spectrogram.

        The device runs the preprocessor continuously, so its noise estimate and PCAN
        gain have adapted to the ambient sound before a word arrives. `warmup` (int16
        audio played before the clip, outputs discarded) reproduces that state.
        """
        if audio.shape[0] < cfg.CLIP_SAMPLES:
            audio = np.pad(audio, (0, cfg.CLIP_SAMPLES - audio.shape[0]))
        self._interp.reset()
        if warmup is not None:
            for start in range(0, len(warmup) - cfg.WINDOW_SAMPLES + 1, cfg.STRIDE_SAMPLES):
                self.frame(warmup[start:start + cfg.WINDOW_SAMPLES])
        out = np.empty((cfg.FEATURE_FRAMES, cfg.FEATURE_CHANNELS), dtype=np.int8)
        for i in range(cfg.FEATURE_FRAMES):
            start = i * cfg.STRIDE_SAMPLES
            out[i] = self.frame(audio[start:start + cfg.WINDOW_SAMPLES])
        return out
