"""Run an int8 KWS model with the TFLM runtime, exactly as the firmware does."""
import numpy as np
from tflite_micro.python.tflite_micro import runtime


class Int8Model:
    def __init__(self, tflite: bytes):
        self.it = runtime.Interpreter.from_bytes(tflite)
        i = self.it.get_input_details(0)["quantization_parameters"]
        o = self.it.get_output_details(0)["quantization_parameters"]
        self.in_scale, self.in_zp = float(i["scales"][0]), int(i["zero_points"][0])
        self.out_scale, self.out_zp = float(o["scales"][0]), int(o["zero_points"][0])
        self.shape = self.it.get_input_details(0)["shape"]

    def requantize(self, feats):
        # Mirrors build_requant_lut() in kws_inference.cc: round half away from zero.
        v = feats.astype(np.float32) / np.float32(self.in_scale)
        q = np.sign(v) * np.floor(np.abs(v) + 0.5) + self.in_zp
        return np.clip(q, -128, 127).astype(np.int8)

    def raw(self, feats):
        self.it.set_input(self.requantize(feats).reshape(self.shape), 0)
        self.it.invoke()
        return self.it.get_output(0).ravel().copy()

    def probs(self, feats):
        return (self.raw(feats).astype(np.float32) - self.out_zp) * np.float32(self.out_scale)
