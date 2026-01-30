# H.264 Pull Decoding (C++ + OpenH264)

Goal: Decode SilkCast H.264 streams (Annex-B) on the receiver side using OpenH264, available for direct consumption by other C++ components, avoiding OpenCV/FFmpeg dependencies.

## Code Location
- `clients/cpp/h264_pull.cpp`: Uses `cpp-httplib` as the HTTP client, parses chunked Annex-B, and calls `OpenH264` to decode.
- `clients/cpp/CMakeLists.txt`: Minimal build script, searches for system or `OPENH264_ROOT` headers/libraries.

## Build
```bash
cd clients/cpp
cmake -S . -B build
cmake --build build
```
Prerequisite: `libopenh264` installed on the system (or set `OPENH264_ROOT` pointing to the directory containing `include/wels/codec_api.h` and the corresponding `lib`). `cpp-httplib` is automatically fetched via `FetchContent` without extra dependencies.

## Run
```bash
./build/h264_pull <host> <port> <device>
# Example: ./build/h264_pull 192.168.1.50 8080 video0
```
The client sends a GET request to `/stream/live/{device}?codec=h264&w=1280&h=720&fps=30`, parses decodable NALs, and calls OpenH264. It prints the current framerate and resolution every 30 frames. The decoded I420 plane data can be passed to downstream processing (e.g., rendering, inference) at the marked Hook location.

## Streaming Parsing Notes
- SilkCast H.264 output is Annex-B (starting with `00 00 00 01`). `AnnexBParser` splits NALs by start code and feeds them one by one into `DecodeFrameNoDelay`. OpenH264 outputs a complete frame after accumulating enough slices.
- The decoding callback exposes YUV plane pointers and stride, facilitating zero-copy docking with hardware or custom processing chains (Note: buffers are held by OpenH264 and must be consumed or copied before the next decode).

## Integration Tips
- For lower latency, keep only the latest frame at the Hook and discard old frames, or use a lock-free ring buffer downstream.
- For WebSocket / UDP pulling, the idea is similar: split by Annex-B start code and call the same decoder to reuse logic.
