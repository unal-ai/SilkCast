# SilkCast (Project Agent Context)

## 1. Project Identity
* **Name:** SilkCast
* **Mission:** To build a foundational **Real-Time Streaming Kernel**.
* **Core Philosophy:**
    * **"Just GET":** Zero friction. If you know the device ID, you can stream it.
    * **On-Demand:** Resources (Cameras/Encoders) spin up when accessed and sleep when idle.
    * **Hybrid Transport:** HTTP/WS for easy viewing, UDP for raw real-time control.

## 2. Notice for Agents (Protocol)
> **CRITICAL INSTRUCTIONS:**
> 1.  **Lazy Loading:** Your code must handle "Hot Path" initialization. If a user GETs a stream that isn't running, start it immediately.
> 2.  **Concurrency:** Use reference counting. Do not shut down the camera if another client is still streaming.
> 3.  **Idempotency:** Multiple GET requests with the same params should share the same underlying encoder session.

---

## 3. The "Just GET" API (RESTful)

### A. Direct Stream Access (HTTP/WebSocket)
*The primary way to view a stream. No setup required.*

| Method | Endpoint | Description |
| :--- | :--- | :--- |
| `GET` | `/stream/live/{id}` | **Instant Stream.** Auto-starts device if off. <br> *Returns:* HTTP-FLV or MJPEG stream (based on accept header). |
| `GET` | `/stream/ws/{id}` | **WebSocket Stream.** Auto-starts device. <br> *Returns:* Binary frames over WebSocket. |
| **Params** | `?w=1920&h=1080&fps=30` | *Optional.* Configures the capture if not already running. |

*Example:* `GET /stream/live/video0?w=1280&fps=60`

### B. UDP / Tele-Op Trigger
*Since UDP is connectionless, we use a lightweight GET to "aim" the cannon.*

| Method | Endpoint | Description |
| :--- | :--- | :--- |
| `GET` | `/stream/udp/{id}` | Tells kernel: "Start blasting UDP packets to my IP". <br> *Params:* `?port=5000&target=192.168.1.5` |

### C. Device & Info (Discovery)
*Optional. Only used if you don't know your Device ID.*

| Method | Endpoint | Description |
| :--- | :--- | :--- |
| `GET` | `/device/list` | Returns JSON list of available IDs (e.g., `["video0", "usb-123"]`). |
| `GET` | `/stream/{id}/stats` | Current FPS, Bitrate, and Viewer Count. |

---

## 4. Architecture Specifications

### A. The Lazy Kernel (Daemon)
* **Session Manager:**
    * Holds a map: `Map<DeviceID, SharedSession>`.
    * On `GET`: Checks map.
        * **Hit:** Returns existing stream.
        * **Miss:** creating `new SharedSession(deviceID)`, starts V4L2 capture, adds to map.
* **Auto-Cleanup:**
    * Every session has a `last_accessed` timestamp.
    * Background thread checks every 10s. If `active_clients == 0`, close device and free memory.

### B. Configuration Logic (The "First Comer" Rule)
* Since multiple users share one camera, who decides the resolution?
* **Rule:** The first person to wake up the camera sets the resolution.
* **Smart Scaling:** If User A started 1080p, and User B requests 720p, the kernel serves User B via a software scaler (or just sends 1080p if bandwidth allows), but does **not** restart the hardware capture.

---

## 5. Implementation Roadmap (Spec-Kit)

### Phase 1: The "Hello World" Stream
- [ ] **Scaffold:** Set up C++ project with `cpp-httplib`.
- [ ] **Capture:** Implement a simple V4L2 class that opens `/dev/video0`.
- [ ] **Endpoint:** Implement `GET /stream/live/{id}` that:
    1. Opens camera.
    2. Reads 1 frame.
    3. Returns it as `Content-Type: image/jpeg` (Snapshot mode).

### Phase 2: The Continuous Stream (MJPEG/FLV)
- [ ] **Threading:** Separate Capture Thread (producer) vs HTTP Thread (consumer).
- [ ] **Chunked Transfer:** Implement HTTP Chunked encoding to keep the `GET` request open indefinitely.

### Phase 3: The "Real-Time" UDP
- [ ] **UDP Emitter:** Add the `/stream/udp/{id}` endpoint that forks a UDP sender thread.

---

## 6. Trial & Error Log
*Format: `[Date] [Agent-ID] [Task] -> [Result] (Notes)`*

* *[XX-XXX-2026] [Genesis] [API Design] -> [Simplified]* Removed all POST/CREATE logic. Adopted "On-Demand" GET architecture for zero-friction access.
* *(New Agents: Add your entry here)*

---

## 7. Production Readiness Addendum

### A. Inputs & Modes
* Treat inputs as adapters: V4L2 cams, screen mirror, RTSP ingest, future sensors all share the same session map and lazy-start rules.
* Latency tiers: `view` (HTTP-FLV/MJPEG/WS), `low` (QUIC/WebRTC datachannel when available), `ultra` (raw UDP with optional tiny FEC). Clients pick via `?latency=`; default `view`.

### B. API Contract Clarifications
* Param arbitration: first-comer locks capture; later requests get software-scaled or codec-transcoded if needed. Return `Effective-Params` header/body describing actual `{w,h,fps,codec,latency}`.
* Error codes: `404` unknown device, `409` incompatible params, `423` device locked, `429` client limit, `503` open/encode failure. Error body: `{"error":"string","details":"..."}`.
* Join path: on attach, force an IDR/I-frame to minimize startup latency; keep a small multi-reader ring to serve the latest IDR quickly.

### C. Control Plane for Other Processes
* Provide a local IPC (Unix socket or gRPC/Protobuf) with commands: `open`, `attach_client`, `detach_client`, `set_profile`, `snapshot`, `stats`, `teardown`.
* Keep IPC as the single source of truth so web, CLI, or other services stay in sync with the session map.

### D. Concurrency & Cleanup Semantics
* State machine: `Idle → Warming → Live → Draining → Idle`; refcounts attach/detach drive transitions.
* Idle timeout definition: `last_byte_sent` or `last_client_heartbeat` (not just socket open). Reaper runs every 10s; log each teardown reason.
* Backpressure: protect IDR frames; drop oldest P/B on UDP/WS when queues exceed threshold; never stall all clients for one slow reader.

### E. Security & Limits
* Optional shared-token header, per-device client caps, and rate limits on `/stream/*`.
* CORS stance documented (default deny); warn that GET activates hardware.

### F. Observability
* Metrics per session: `startup_ms`, `first_iframe_ms`, `fps_out`, `bitrate_out`, `queue_depth`, `drop_pct`, `active_clients`, `uptime_s`.
* Structured logs include `device_id`, `session_id`, `state`, `reason`, and params; `/stream/{id}/stats` must read from the same counters.

### G. Testing & CI
* Unit tests: session map refcounts, idle reaper, first-comer param lock, latency-tier selection, forced-IDR join.
* Integration smoke: loopback UDP latency test, HTTP-FLV long-poll stability, IPC contract test, screen-mirror adapter test.
* Build matrix: Linux first; note required deps (`cpp-httplib`, V4L2, encoder libs).

---

## 8. Implementation Notes (Current)
- Capture path uses V4L2; pixel format chosen by first requester: `codec=mjpeg` → MJPEG, `codec=h264` → YUYV (converted to I420).
- H.264 encoding via optional OpenH264 (Cisco binary recommended for patent coverage); Annex-B NALs streamed over HTTP chunked.
- MJPEG and H.264 share lazy sessions; codec mismatches return 409 with `Effective-Params`.
- CLI flags: `--addr`, `--port`, `--idle-timeout`, `--codec`. Desktop launcher: `scripts/launch_desktop.sh` builds then opens `/device/list`.
- Packaging: `scripts/build_linux.sh` for amd64/arm64; systemd unit at `packaging/systemd/silkcast.service`. Non-Linux builds stub capture.
- I420 conversion is foundational; keep a fast YUYV→I420 path and avoid buffering (“latest frame only”) for preview/tele-op use. 
- Stats: `/stream/{id}/stats` returns fps/bitrate estimates based on sent frames/bytes; session tracks frames/bytes/clients and resets on first start. IDR forced on client join for H.264.
- WebSocket: `/stream/ws?id={id}&codec=...` streams binary MJPEG or H.264 NALs.
- UDP: `/stream/udp/{id}?target=IP&port=5000&codec=h264&duration=10` sends best-effort UDP (Annex-B or MJPEG) for ultra-low latency; Linux only, kernel fragmentation (MTU best effort).
- fMP4: `/stream/live/{id}?codec=h264&container=mp4` returns chunked fragmented MP4 (tiny fragments, Baseline, IDR on join). CORS + no-store headers applied.
- OpenH264 fetch: Enabled by default. `AUTO_FETCH_OPENH264=ON` auto-downloads Cisco v2.6.0 binary+headers on Linux x86_64/arm64; else set `OPENH264_ROOT` or system install. Disable with `-DENABLE_OPENH264=OFF` if licensing blocks usage.
