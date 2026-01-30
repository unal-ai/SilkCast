"""
Lightweight SilkCast pull client (MJPEG over HTTP).

Goals:
* Avoid OpenCV/FFmpeg on the receiver; depend only on requests and optional Pillow/TurboJPEG.
* Keep latency low by parsing the multipart MJPEG stream incrementally.
* Provide a simple generator interface so other components can plug in quickly.

Usage
-----
from clients.python.silkcast_client import SilkCastMJPEGClient

client = SilkCastMJPEGClient(base_url="http://capture-host:8080",
                             device="video0",
                             params={"w": 640, "h": 480, "fps": 30},
                             decode="pillow")  # or "turbojpeg" or None
for frame in client.frames():
    rgb = frame.decoded  # numpy array if decode enabled, else None
    process(rgb or frame.jpeg)
"""
from __future__ import annotations

import io
import time
from dataclasses import dataclass
from typing import Callable, Dict, Generator, Optional, Union

import requests


@dataclass
class Frame:
    """Represents one MJPEG frame pulled from SilkCast."""

    seq: int
    timestamp: float
    jpeg: bytes
    decoded: Optional["np.ndarray"] = None  # populated when decode is enabled


Decoder = Union[str, Callable[[bytes], object], None]


class SilkCastMJPEGClient:
    """
    Minimal MJPEG puller for SilkCast.

    Notes:
    - Uses the existing `/stream/live/{device}` endpoint with `codec=mjpeg`.
    - Parses `multipart/x-mixed-replace` by scanning JPEG SOI/EOI markers,
      which keeps latency low and avoids buffering whole chunks.
    """

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8080",
        device: str = "video0",
        params: Optional[Dict[str, Union[str, int]]] = None,
        timeout: float = 5.0,
        decode: Decoder = None,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.device = device
        self.params = params or {}
        self.timeout = timeout
        self.decode = decode
        self._resp: Optional[requests.Response] = None
        self._stop = False
        self._seq = 0
        self._decoder_impl = None  # lazy-initialized decoder

    def _url(self) -> str:
        return f"{self.base_url}/stream/live/{self.device}"

    def _ensure_decoder(self):
        """Set up lazy decoder based on `decode` preference."""
        if self._decoder_impl is not None or not self.decode:
            return
        if callable(self.decode):
            self._decoder_impl = self.decode
            return
        if self.decode == "turbojpeg":
            from turbojpeg import TJPF_RGB, TurboJPEG  # type: ignore

            tj = TurboJPEG()
            self._decoder_impl = lambda b: tj.decode(b, pixel_format=TJPF_RGB)
        elif self.decode == "pillow":
            from PIL import Image  # type: ignore
            import numpy as np  # type: ignore

            def _decode(b: bytes):
                return np.array(Image.open(io.BytesIO(b)).convert("RGB"))

            self._decoder_impl = _decode
        else:
            self._decoder_impl = None

    def _open(self) -> None:
        """Open the HTTP stream."""
        headers = {"Accept": "multipart/x-mixed-replace"}
        # Force codec=mjpeg unless caller overrides explicitly.
        req_params = {"codec": "mjpeg", **self.params}
        self._resp = requests.get(
            self._url(), params=req_params, stream=True, timeout=self.timeout, headers=headers
        )
        if self._resp.status_code != 200:
            body = self._resp.text[:200]
            self.close()
            raise RuntimeError(f"SilkCast responded {self._resp.status_code}: {body}")

    def frames(self) -> Generator[Frame, None, None]:
        """
        Stream frames as they arrive.

        Yields:
            Frame objects containing the raw JPEG bytes and optional decoded array.
        """
        if self._resp is None:
            self._open()
        assert self._resp is not None
        self._ensure_decoder()

        buffer = b""
        decoder = self._decoder_impl
        for chunk in self._resp.iter_content(chunk_size=4096):
            if self._stop:
                break
            if not chunk:
                continue
            buffer += chunk
            # Extract all complete JPEGs present in the buffer.
            while True:
                start = buffer.find(b"\xff\xd8")  # JPEG SOI
                if start == -1:
                    # No start marker found yet.
                    buffer = buffer[-3:]  # keep tail to catch split marker
                    break
                end = buffer.find(b"\xff\xd9", start + 2)  # JPEG EOI
                if end == -1:
                    # Incomplete JPEG; keep the buffer.
                    buffer = buffer[start:]
                    break
                jpeg_bytes = buffer[start : end + 2]
                buffer = buffer[end + 2 :]

                decoded = decoder(jpeg_bytes) if decoder else None
                yield Frame(seq=self._seq, timestamp=time.time(), jpeg=jpeg_bytes, decoded=decoded)
                self._seq += 1

        self.close()

    def grab_one(self) -> Optional[Frame]:
        """Fetch a single frame then close the stream."""
        for frame in self.frames():
            self.close()
            return frame
        return None

    def close(self) -> None:
        """Close the HTTP stream."""
        self._stop = True
        if self._resp is not None:
            try:
                self._resp.close()
            finally:
                self._resp = None


__all__ = ["SilkCastMJPEGClient", "Frame"]
