# SilkCast Receiver (No OpenCV/FFmpeg dependency)

Goal: Directly pull SilkCast MJPEG streams on the receiver side, achieving ultra-low latency input for components (like Virtual Mouse) without introducing OpenCV/FFmpeg.

## Quick Start
1) Start SilkCast Sender (Example)
`./build/silkcast --addr 0.0.0.0 --port 8080`

2) Install Lightweight Dependencies
`pip install requests`
Optional Decoding: `pip install pillow` or `pip install turbojpeg`

3) Usage Example
```python
from clients.python.silkcast_client import SilkCastMJPEGClient

client = SilkCastMJPEGClient(
    base_url="http://capture-host:8080",
    device="video0",
    params={"w": 640, "h": 480, "fps": 30},
    decode="pillow",  # "turbojpeg" | "pillow" | None | custom callable
)

for frame in client.frames():  # streaming generator
    rgb = frame.decoded  # numpy array if decode enabled
    do_something(rgb or frame.jpeg)
```

## Virtual Mouse Integration Approach
- Existing `VideoInput` relies on RTSP + OpenCV. It can be replaced with `SilkCastMJPEGClient`, keeping the "read latest frame" semantics.
- Typical Adaptation Steps:
  1. Start SilkCast: `GET /stream/live/video0?codec=mjpeg&w=640&h=480&fps=30` (First request activates camera, subsequent ones share the session).
  2. Use `client.frames()` in the input processing thread to iterate frames; if "latest frame" mode is needed, only keep the newest `frame` in the loop and discard old ones to reduce latency.
  3. If the caller still needs a `numpy` array, enable `decode="pillow"` (lighter than OpenCV). For purely decode-free scenarios, pass `frame.jpeg` bytes directly.

## Run Parameters
- `base_url`: SilkCast service address, e.g., `http://localhost:8080` or remote IP.
- `device`: Device ID (from `/device/list`).
- `params`: Width, height, FPS, etc., e.g., `{"w":1280,"h":720,"fps":60}`. The first awakener locks hardware params; subsequent requests reuse and software-scale.
- `decode`: `None` (default, returns raw JPEG), `"pillow"`, `"turbojpeg"`, or custom `Callable[jpeg_bytes -> object]`.
- `timeout`: HTTP connection timeout, default 5s.

## Notes
- MJPEG pulling uses `multipart/x-mixed-replace; boundary=frame`; clients just parse JPEG SOI/EOI.
- For fMP4/H.264, use `codec=h264&container=mp4`, but this example client targets MJPEG; H.264 pulling can be handled by hardware decoders or lighter dedicated libraries.
- The server returns `Effective-Params` header, indicating actual `{w,h,fps,codec,container}`, useful for parameter arbitration debugging.

## Directory
- Code: `clients/python/silkcast_client.py`
- Docs: `docs/pull_client.md` (This file)
