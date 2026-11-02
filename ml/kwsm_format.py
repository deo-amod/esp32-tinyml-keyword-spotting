"""Reader/writer for the .kwsm model container (header + TFLite flatbuffer).

Layout must match firmware/components/kws_model/include/kws_model_format.h.
"""
import hashlib
import struct
import zlib
from dataclasses import dataclass, field

MAGIC = 0x4D53574B  # b"KWSM" little-endian
HEADER_VERSION = 1
HEADER_SIZE = 256
MAX_LABELS = 8
LABEL_LEN = 16
SIG_MAX = 80
SIGNED_PREFIX = 164  # bytes of header covered by the signature (everything before it)

_FMT = "<IHHIIIHHHHIBBxx128s80s8xI"
assert struct.calcsize(_FMT) == HEADER_SIZE


@dataclass
class ModelHeader:
    model_version: int
    model_size: int
    model_crc32: int
    feature_frames: int
    feature_channels: int
    window_ms: int
    stride_ms: int
    sample_rate: int
    labels: list = field(default_factory=list)
    signature: bytes = b""

    def _pack(self, header_crc=0):
        if len(self.labels) > MAX_LABELS:
            raise ValueError("too many labels")
        labels = b"".join(l.encode()[:LABEL_LEN - 1].ljust(LABEL_LEN, b"\0") for l in self.labels)
        if len(self.signature) > SIG_MAX:
            raise ValueError("signature too long")
        return struct.pack(_FMT, MAGIC, HEADER_VERSION, HEADER_SIZE, self.model_version,
                           self.model_size, self.model_crc32, self.feature_frames,
                           self.feature_channels, self.window_ms, self.stride_ms,
                           self.sample_rate, len(self.labels), len(self.signature),
                           labels.ljust(MAX_LABELS * LABEL_LEN, b"\0"),
                           self.signature.ljust(SIG_MAX, b"\0"), header_crc)

    def pack(self) -> bytes:
        raw = self._pack()
        return raw[:-4] + struct.pack("<I", zlib.crc32(raw[:-4]))

    def signed_digest(self, tflite: bytes) -> bytes:
        return hashlib.sha256(self._pack()[:SIGNED_PREFIX] + tflite).digest()


def build(tflite: bytes, version: int, labels, cfg, sign=None) -> bytes:
    """Return a complete .kwsm blob. `sign(digest) -> DER signature` is optional."""
    hdr = ModelHeader(version, len(tflite), zlib.crc32(tflite), cfg.FEATURE_FRAMES,
                      cfg.FEATURE_CHANNELS, cfg.WINDOW_MS, cfg.STRIDE_MS, cfg.SAMPLE_RATE,
                      list(labels))
    if sign is not None:
        hdr.signature = sign(hdr.signed_digest(tflite))
    return hdr.pack() + tflite


def parse(blob: bytes):
    f = struct.unpack(_FMT, blob[:HEADER_SIZE])
    (magic, hver, hsize, mver, msize, mcrc, frames, chans, win, stride, sr, nlab, siglen,
     labels, sig, hcrc) = f
    if magic != MAGIC or hver != HEADER_VERSION or hsize != HEADER_SIZE:
        raise ValueError("not a kwsm v1 blob")
    if zlib.crc32(blob[:HEADER_SIZE - 4]) != hcrc:
        raise ValueError("header CRC mismatch")
    tflite = blob[HEADER_SIZE:HEADER_SIZE + msize]
    if len(tflite) != msize or zlib.crc32(tflite) != mcrc:
        raise ValueError("model CRC mismatch")
    names = [labels[i * LABEL_LEN:(i + 1) * LABEL_LEN].split(b"\0")[0].decode() for i in range(nlab)]
    hdr = ModelHeader(mver, msize, mcrc, frames, chans, win, stride, sr, names, sig[:siglen])
    return hdr, tflite
