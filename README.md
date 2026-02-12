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
- `GET /capabilities` (single-source capability snapshot: codecs, containers, media kinds, adapter registrations)
- `GET /stream/live/{id}?media=video&codec=mjpeg&fps=15` (real V4L2 MJPEG capture; first request locks params)
- `GET /device/{id}/caps` (V4L2 native formats, resolutions, and frame intervals; Linux only)
- `GET /stream/live/{id}?codec=h264&container=mp4` (chunked fMP4: Baseline, IDR on join, tiny fragments)
- `GET /stream/{id}/stats`
- `GET /stream/ws/{id}` and `GET /stream/ws?id={id}` (currently returns `501 not_implemented` in this `cpp-httplib v0.15.x` build; reserved for WS transport)
- `GET /stream/udp/{id}?target=IP&port=5000&codec=h264&duration=10` (best-effort UDP; Linux only; MTU-frag by kernel)
- Capability negotiation uses strong central registries (`src/capability_registry.*` + `src/adapter_registry.*`), so codec/container/media/transport support is defined in one place.

### H.264 / OpenH264 (compatibility default)
- H.264 Baseline is enabled by default for broad compatibility; disable with `-DENABLE_OPENH264=OFF` if licensing is a concern.
- On Linux x86_64/arm64, we auto-fetch Cisco’s official v2.6.0 binary + headers during CMake if `AUTO_FETCH_OPENH264=ON` (default).
### Lightweight pull clients
- `docs/pull_client.md`: Python MJPEG receiver, without dependency of OpenCV/FFmpeg。
- `docs/h264_pull_client.md`: C++ H.264 pull stream + OpenH264 decoding example, convenient for low-latency processing chain.

### H.264 / OpenH264 (compatibility default)
- H.264 Baseline is enabled by default for broad compatibility; disable with `-DENABLE_OPENH264=OFF` if licensing is a concern.
- On Linux x86_64/arm64, we auto-fetch Cisco’s official v2.6.0 binary + headers during CMake if `AUTO_FETCH_OPENH264=ON` (default). This keeps the Cisco binary license path intact.
- Otherwise, set `OPENH264_ROOT=/path/to/openh264` or install `libopenh264` system-wide.
- Flow: capture YUYV → I420 → OpenH264 (Baseline, low-delay); HTTP chunked delivers Annex-B NALs, or fMP4 if `container=mp4`.
- Future room: H.265/AV1 can be added later behind optional builds—nothing claimed or shipped yet.

### CLI flags
- `--addr <ip>` bind address (default `0.0.0.0`)
- `--port <port>` bind port (default `8080`)
- `--idle-timeout <s>` idle seconds before device teardown (default `10`)
- `--codec <mjpeg|h264>` default codec when not specified (default `mjpeg`)

### Desktop launcher (demo)
`scripts/launch_desktop.sh` builds, runs, then opens the demo UI at `/`.
Override behavior with environment variables:
- `DEMO_MODE=stream` to open a stream directly.
- `DEMO_MODE=list` to open `/device/list` instead.
- `STREAM_DEVICE=video1` to pick a specific device.
- `STREAM_CODEC=mjpeg` or `STREAM_PARAMS=codec=mjpeg&fps=15` to control stream.
- `SKIP_BUILD=1` to skip rebuilds.

### Requirements
- Linux with V4L2 camera (e.g., `/dev/video0`); package `v4l-utils` recommended for debugging. Non-Linux builds compile but camera capture stubs out.
- No Docker required; single binary.

### Builds (Linux amd64/arm64)
- Helper: `ARCH=arm64 ENABLE_H264=1 ./scripts/build_linux.sh`
- For mostly-static builds add `-DBUILD_SHARED_LIBS=OFF` (toolchain permitting). Fully static glibc is not guaranteed.

## Service (Linux)
A sample unit lives at `packaging/systemd/silkcast.service`. Install the built binary to `/usr/local/bin/silkcast`, create `silkcast` user/group, and `systemctl enable --now silkcast`.

## License
Apache License 2.0 — see `LICENSE`.
