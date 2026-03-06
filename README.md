SilkCast is a zero-config real-time streaming kernel: one GET wakes, streams, and shares any camera or screen over HTTP/WS/UDP with latency tiers that fit labs, remote tele-op, and quick viewing.  
Built to be both a foundation and a drop-in endpoint, it scales from single USB cams to mixed inputs like screen mirroring with the same lazy-start, shared-session core.

## Quickstart (dev)
```bash
cmake -S . -B build
cmake --build build
./build/silkcast
# or: scripts/launch_desktop.sh   # double-click friendly, opens the demo UI
```
Endpoints (early stub):
- `GET /device/list`
- `GET /system/info`
- `GET /capabilities` (single-source capability snapshot: codecs, containers, raw formats, media kinds, adapter registrations)
- `GET /stream/live/{id}?media=video&codec=mjpeg&fps=15` (real V4L2 MJPEG capture; first request locks params)
- `GET /stream/live/{id}?codec=raw&container=raw` (multipart raw frames; `pixfmt=i420|rgb24`)
- `GET /device/{id}/caps` (native device capabilities: V4L2 formats on Linux, AVFoundation formats on macOS)
- `GET /stream/live/{id}?codec=h264&container=mp4` (chunked fMP4: Baseline, IDR on join, tiny fragments)
- `GET /stream/{id}/stats`
- `ws://{host}:{ws_port}/stream/ws/{id}` and `ws://{host}:{ws_port}/stream/ws?id={id}` (binary WS frames; shared lazy session/refcount path, including `codec=raw&pixfmt=i420|rgb24`)
- `GET /stream/ws/{id}` and `GET /stream/ws?id={id}` (HTTP hint route; returns `426 upgrade_required` + `ws_url`)
- `GET /stream/udp/{id}?target=IP&port=5000&codec=h264&duration=10` (best-effort UDP; Linux only; MTU-frag by kernel)
- Capability negotiation uses strong central registries (`src/capability_registry.*` + `src/adapter_registry.*`), so codec/container/media/transport support is defined in one place.

### RTSP Relay (no ffmpeg in runtime path)
SilkCast can pull RTSP and relay to browsers with chunked fMP4 or raw Annex-B.  
Treat the RTSP URL as `{id}` and URL-encode it in the path:

- Browser-ready fMP4:
  - `GET /stream/live/rtsp%3A%2F%2F192.168.1.50%3A8554%2Fstream?codec=h264&container=mp4`
- Raw Annex-B:
  - `GET /stream/live/rtsp%3A%2F%2F10.0.0.12%2Flive.sdp?codec=h264&container=raw`

Notes:
- If `{id}` starts with `rtsp://`, defaults are `codec=h264`, `container=mp4`, and ultra-latency preset unless explicitly overridden.
- `codec=raw` is for local capture sources only; RTSP inputs stay encoded in this stack.
- RTSP keepalive (`OPTIONS`/`GET_PARAMETER`) is sent automatically.
- No ffmpeg/GStreamer dependency is required for the server relay pipeline.

### RAW output
- `GET /stream/live/video0?codec=raw&container=raw&pixfmt=i420&w=640&h=480&fps=30`
  Returns `multipart/x-mixed-replace`; each part body is exactly one I420 frame.
- `GET /stream/live/video0?codec=raw&container=raw&pixfmt=rgb24&w=640&h=480&fps=30`
  Returns `multipart/x-mixed-replace`; each part body is exactly one packed RGB24 frame.
- `ws://{host}:{ws_port}/stream/ws/video0?codec=raw&container=raw&pixfmt=i420&w=640&h=480&fps=30`
  Sends one binary WebSocket message per I420 frame.
- `ws://{host}:{ws_port}/stream/ws/video0?codec=raw&container=raw&pixfmt=rgb24&w=640&h=480&fps=30`
  Sends one binary WebSocket message per RGB24 frame.
- RAW sessions expose metadata via headers:
  - `X-SilkCast-Codec: raw`
  - `X-SilkCast-Pixel-Format: i420` or `rgb24`
  - `X-SilkCast-Width`, `X-SilkCast-Height`, `X-SilkCast-Fps`
  - `X-SilkCast-Frame-Bytes`
- Current scope keeps transport shape fixed while allowing `I420` or `RGB24` payloads from local capture sources.

### Lightweight pull clients
- `docs/pull_client.md`: Python MJPEG receiver without OpenCV/FFmpeg dependency.
- `docs/h264_pull_client.md`: C++ H.264 pull stream + OpenH264 decode example for low-latency pipelines.

### H.264 / OpenH264 (compatibility default)
- H.264 Baseline is enabled by default for broad compatibility; disable with `-DENABLE_OPENH264=OFF` if licensing is a concern.
- On Linux x86_64/arm64, we auto-fetch Cisco’s official v2.6.0 binary + headers during CMake if `AUTO_FETCH_OPENH264=ON` (default). This keeps the Cisco binary license path intact.
- Otherwise, set `OPENH264_ROOT=/path/to/openh264` or install `libopenh264` system-wide.
- Flow: capture YUYV → I420 → OpenH264 (Baseline, low-delay); HTTP chunked delivers Annex-B NALs, or fMP4 if `container=mp4`.
- Future room: H.265/AV1 can be added later behind optional builds—nothing claimed or shipped yet.

### CLI flags
- `--addr <ip>` bind address (default `0.0.0.0`)
- `--port <port>` bind port (default `8080`)
- `--ws-port <port>` websocket sidecar port (default `port+1`, use `0` to disable)
- `--idle-timeout <s>` idle seconds before device teardown (default `10`)
- `--codec <mjpeg|h264|raw>` default codec when not specified (default `mjpeg`)

### Desktop launcher (demo)
`scripts/launch_desktop.sh` builds, runs, then opens the demo UI at `/`.
Override behavior with environment variables:
- `DEMO_MODE=stream` to open a stream directly.
- `DEMO_MODE=list` to open `/device/list` instead.
- `STREAM_DEVICE=video1` to pick a specific device.
- `STREAM_CODEC=mjpeg` or `STREAM_PARAMS=codec=mjpeg&fps=15` to control stream.
- `SKIP_BUILD=1` to skip rebuilds.

### RTSP smoke test (optional local validation)
`scripts/rtsp_smoke.sh` spins up `mediamtx` + a synthetic RTSP publisher (`ffmpeg`), starts SilkCast, requests encoded RTSP relay over fMP4, and validates `ftyp` output.

### API smoke test (fast contract check)
`scripts/smoke_api.sh` validates key HTTP contracts (`/system/info`, `/capabilities`, bad-param `400`, websocket upgrade hints + sidecar listener behavior, encoded RTSP route + stats lifecycle fields).

### Requirements
- Linux with V4L2 camera (e.g., `/dev/video0`); package `v4l-utils` recommended for debugging.
- macOS uses AVFoundation capture path and supports `/device/{id}/caps` capability introspection.
- No Docker required; single binary.

### Builds (Linux amd64/arm64)
- Helper: `ARCH=arm64 ENABLE_H264=1 ./scripts/build_linux.sh`
- For mostly-static builds add `-DBUILD_SHARED_LIBS=OFF` (toolchain permitting). Fully static glibc is not guaranteed.

## Service (Linux)
A sample unit lives at `packaging/systemd/silkcast.service`. Install the built binary to `/usr/local/bin/silkcast`, create `silkcast` user/group, and `systemctl enable --now silkcast`.

## License
Apache License 2.0 — see `LICENSE`.
