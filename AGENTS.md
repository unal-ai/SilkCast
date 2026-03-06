# SilkCast (Project Agent Context)

## 0. TL;DR (Start Here)
If you only have 2 minutes, align on these points first:
- SilkCast is a lazy, shared, real-time streaming kernel: `GET` should be enough to wake and stream a device.
- Never break the hot path invariants: lazy start, shared session reuse, and refcount-safe teardown.
- Current execution order: session correctness first, continuous streaming stability second, transport breadth third.
- Keep extension seams stable: reserve codec and adapter slots now (`h265`/`av1`, audio stream, virtual camera, virtual microphone) without forcing immediate feature delivery.
- Treat `AGENTS.md` as a living protocol: update it proactively when project reality changes.
- Every non-trivial change must include a rollback path and an entry in the decision log (Section 6).

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

### 2.1 Living AGENTS.md Protocol
- Treat this file as executable collaboration state, not static documentation.
- Agents are expected to update protocol rules, current assumptions, and implementation notes whenever they become stale.
- Prefer bold but coherent edits: simplify, reorder, or rewrite sections when it improves clarity and team execution speed.
- When behavior changes in code, update `AGENTS.md` in the same change set with rationale, impact, and rollback path.
- Keep this file English-only, concrete, and date-accurate.

### 2.2 Traditional Systems Engineering Style
- Primary implementation style is traditional systems engineering: C++ for core runtime, C/POSIX APIs for low-level integration, and Unix shell for build/deploy glue.
- Prefer explicit state machines, deterministic behavior, and clear resource ownership (RAII, scoped lifetimes, predictable teardown).
- Favor tool-friendly Unix workflows: CLI-first control, text-based configs, structured logs, and reproducible scripts.
- Keep dependencies minimal and portable; choose stable libraries and standards that support long-lived maintenance.
- Use concise professional technical English in code comments, docs, API messages, and logs.
- Keep source and docs formatting straightforward and terminal-friendly.

### 2.3 Stage Goals (Current)
1. Ship a stable "Just GET" experience for HTTP/WS/UDP with lazy start and shared sessions.
2. Keep first-comer param lock predictable while serving later clients via scaling/transcoding when possible.
3. Make teardown behavior deterministic (`active_clients`, idle timeout, and explicit state transitions).
4. Keep observability and stats accurate enough to debug startup, join, and throughput behavior quickly.
5. Preserve forward-compatible extension points for future codecs (`h265`, `av1`) and AV I/O adapters (audio stream, virtual camera, virtual microphone).

### 2.4 Stage Non-Goals (Current)
- Do not optimize for advanced multi-node orchestration yet.
- Do not ship production H.265/AV1 encode paths yet unless they directly unblock reliability work; reserve interfaces first.
- Do not add control-plane complexity (auth/policy/plugin systems) before the core streaming path is stable.
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

### 5.1 Priority Queue (Execute Top to Bottom)

#### P0. Session Correctness (Must Pass First)
- [ ] Enforce one lazy-start per unique session key and share it across equivalent GET requests.
- [ ] Maintain correct attach/detach refcount across HTTP, WebSocket, and UDP consumers.
- [ ] Guarantee "no premature shutdown": a session can only drain when `active_clients == 0` and idle timeout expires.

**Definition of Done**
- Repeated identical GET requests reuse a single underlying capture/encoder session.
- Concurrent attach/detach stress test shows no double-open, negative refcount, or stuck live session.
- Reaper logs show explicit teardown reason and only trigger after refcount and timeout conditions are met.

#### P1. Baseline Capture and Snapshot Path
- [ ] Keep V4L2 open/read path stable for known devices (`/device/list` and `/stream/live/{id}` snapshot behavior).
- [ ] Validate first-comer parameter lock on initial open (`w/h/fps/codec`).

**Definition of Done**
- Snapshot path returns valid JPEG for a known camera device.
- Invalid/unknown device IDs return the documented error shape.
- First request locks effective capture params and later requests do not restart hardware capture.

#### P2. Continuous Stream Reliability (MJPEG/H.264/fMP4/WS)
- [ ] Keep producer/consumer threading separation with bounded queues and drop policy for slow readers.
- [ ] Keep chunked responses alive and resilient under long-running clients.
- [ ] Ensure forced IDR on new H.264 joins and consistent stats updates.

**Definition of Done**
- Long-poll client remains stable over an extended run without server deadlock or runaway memory.
- H.264 join path starts quickly with decodable data (IDR on attach).
- `/stream/{id}/stats` values track observed frame/byte flow for active sessions.

#### P3. UDP Tele-Op Path
- [ ] Keep `/stream/udp/{id}` best-effort sender behavior predictable (`target`, `port`, `duration`).
- [ ] Keep UDP path non-blocking so a slow network target does not stall other transports.

**Definition of Done**
- Loopback UDP smoke test receives packets for requested duration window.
- UDP sender teardown is clean and does not leak client refs or worker threads.

#### P4. Extensibility Guardrails (Codecs + AV I/O)
- [ ] Add a codec capability registry contract that can reserve `h265` and `av1` identifiers behind feature flags.
- [ ] Keep unsupported codec requests deterministic (error + list of enabled codecs) rather than ambiguous fallback.
- [ ] Define adapter contracts for media kinds and virtual devices:
  - input: camera/screen/rtsp video plus audio stream sources
  - output: HTTP/WS/UDP plus virtual camera and virtual microphone sinks
- [ ] Keep lazy-start/refcount/idle-teardown semantics identical for `video`, `audio`, and combined `av` sessions.

**Definition of Done**
- Build remains stable with `h265`/`av1` placeholders compiled but disabled by default.
- Requesting a disabled codec (for example `?codec=h265`) returns a documented, explicit "unsupported codec" response.
- Adapter contract documentation lists minimum lifecycle methods (`open`, `attach_client`, `detach_client`, `read/write`, `close`) and capability metadata.

---

## 6. Change & Decision Log (Mandatory)
Every meaningful change must be recorded here using this exact structure:

`[YYYY-MM-DD] [Agent-ID] [Area]`
- **What changed:** Concrete code/config/API behavior changes.
- **Why:** Problem statement or decision rationale.
- **Impact:** User-visible and system-level effects (include metric movement when available).
- **Rollback:** Exact revert path (commit, file set, or command-level strategy).

Example:
- `[2026-01-27] [Genesis] [API Design]`
  - **What changed:** Removed POST/CREATE workflow and switched to on-demand GET activation.
  - **Why:** Reduce client friction and align with "Just GET" kernel behavior.
  - **Impact:** Simplified client integration and session startup path.
  - **Rollback:** Restore previous control endpoints and disable lazy GET activation in router/session manager.

Entries:
- `[2026-02-12] [Codex] [First-IFrame Metric + API Smoke Script]`
  - **What changed:** Added `first_iframe_ms` tracking to session metrics (H.264 paths only) and exposed it in `/stream/{id}/stats`; added `scripts/smoke_api.sh` to verify core HTTP contracts (health/capabilities, bad-param errors, WS placeholder behavior, encoded RTSP route + lifecycle stats fields).
  - **Why:** Improve startup observability for decode-readiness and make regression checks repeatable with a single command.
  - **Impact:** Teams can now distinguish first-byte vs first-I-frame timing and quickly catch API drift before merge.
  - **Rollback:** Revert `src/types.hpp`, `src/stream_utils.*`, `src/main.cpp`, `scripts/smoke_api.sh`, and related README/AGENTS notes.
- `[2026-02-12] [Codex] [Session State Machine + Teardown Reason]`
  - **What changed:** Added explicit session lifecycle enums (`idle/warming/live/draining`) and teardown reasons (`none/idle_timeout/open_failed/runtime_error`) in `src/types.hpp`; wired transitions through live/udp attach-detach flow and reaper cleanup; added startup and first-frame timing counters to session stats; centralized per-frame accounting via `stream::note_frame_sent`.
  - **Why:** Make concurrency and cleanup behavior observable and deterministic, and remove implicit state guesses during debugging.
  - **Impact:** `/stream/{id}/stats` now reports `state`, `teardown_reason`, `startup_ms`, and `first_frame_ms`; reaper logs teardown reason on cleanup; first-frame timing is measured when output actually begins.
  - **Rollback:** Revert `src/types.hpp`, `src/main.cpp`, `src/session_manager.cpp`, and `src/stream_utils.*`.
- `[2026-02-12] [Codex] [API Hardening + Contract Alignment]`
  - **What changed:** Replaced exception-based query parsing with explicit numeric parameter validation (`w/h/fps/bitrate/quality/gop`) and structured `400` JSON errors; added `/stream/ws` query-form placeholder route (`/stream/ws?id=...`) to align behavior with docs; added `clients/cpp/h264_pull.cpp` + `clients/cpp/CMakeLists.txt` so C++ pull-client docs reference real files.
  - **Why:** Prevent runtime `500` from malformed user input, remove API contract drift for WS query path, and eliminate documentation/code mismatch for receiver tooling.
  - **Impact:** Invalid numeric params now fail deterministically with actionable `bad_request` payloads; `GET /stream/ws?id=video0` now returns consistent `501 not_implemented` instead of `404`; C++ receiver onboarding path is now executable.
  - **Rollback:** Revert `src/stream_utils.*`, `src/main.cpp`, `clients/cpp/*`, and `.gitignore`.
- `[2026-02-12] [Codex] [RTSP Relay Integration]`
  - **What changed:** Integrated RTSP ingest path into shared sessions (`CaptureRTSP` + `CaptureInterface`), enabled `rtsp_ingest` adapter registration, added URL-decoded device handling for stream routes, and added `/system/info` plus RTSP relay smoke script `scripts/rtsp_smoke.sh`.
  - **Why:** Make RTSP a first-class input adapter under the same lazy-start/refcount session model while preventing route mismatch for URL-encoded RTSP IDs.
  - **Impact:** `GET /stream/live/{id}` now accepts encoded RTSP IDs (for example `rtsp%3A%2F%2F...`) and reaches the capture path instead of returning route-level `404`; capabilities snapshot now advertises RTSP ingest readiness; teams get a repeatable local relay smoke test.
  - **Rollback:** Revert `src/capture_interface.hpp`, `src/capture_rtsp.*`, `src/session_manager.cpp`, `src/main.cpp`, `src/stream_utils.*`, `src/api_router.hpp`, `src/types.hpp`, `src/adapter_registry.cpp`, `CMakeLists.txt`, and `scripts/rtsp_smoke.sh`.
- `[2026-02-12] [Codex] [AGENTS Governance]`
  - **What changed:** Added onboarding TL;DR, explicit stage goals/non-goals, prioritized roadmap with per-priority DoD, and mandatory change-log template.
  - **Why:** Improve onboarding speed and make execution order, acceptance criteria, and rollback discipline explicit for all contributors.
  - **Impact:** Lower coordination overhead; task handoff and review are now more consistent and measurable.
  - **Rollback:** Revert `AGENTS.md` to the previous revision from git history.
- `[2026-02-12] [Codex] [Extensibility Reservation]`
  - **What changed:** Added explicit reservations for future codecs (`h265`, `av1`) and AV I/O expansion (audio stream, virtual camera, virtual microphone), including roadmap DoD guardrails.
  - **Why:** Prevent architectural lock-in while keeping current delivery focused on stable MJPEG/H.264 paths.
  - **Impact:** Future encoder and media-path work can plug into predefined contracts without redesigning session semantics.
  - **Rollback:** Revert the corresponding `AGENTS.md` sections to the prior revision.
- `[2026-02-12] [Codex] [Protocol + Style Reinforcement]`
  - **What changed:** Added explicit rules that agents should proactively and boldly update `AGENTS.md`, and added a positive traditional systems style guide centered on C++/Unix/C practices.
  - **Why:** Keep collaboration state current and align implementation/output style with long-lived systems project conventions.
  - **Impact:** Faster team convergence on protocol updates and more consistent engineering style across contributions.
  - **Rollback:** Revert the newly added Section 2 protocol/style subsections and this log entry.
- `[2026-02-12] [Codex] [Build Stabilization + Capability Gating]`
  - **What changed:** Restored successful local build by aligning OpenH264 option usage with current headers, fixing non-Linux capture stubs, replacing unavailable WS server registration with explicit HTTP `501` placeholders, and adding deterministic codec/container validation responses (`enabled` + `known` codec lists).
  - **Why:** Keep the project buildable across development hosts and make transport capability limits explicit and machine-readable.
  - **Impact:** `cmake --build` now passes on macOS dev environments; unsupported codec/container requests fail predictably; idle cleanup now follows reaper timeout semantics instead of immediate teardown.
  - **Rollback:** Revert `src/main.cpp`, `src/encoder_h264.cpp`, and `src/capture_v4l2.hpp` to the previous revision.
- `[2026-02-12] [Codex] [Strong Capability Registration]`
  - **What changed:** Introduced centralized capability registration in `src/capability_registry.hpp` + `src/capability_registry.cpp` and routed transport validation through that registry.
  - **Why:** Prevent partial updates and drift caused by scattered codec/container checks across handlers.
  - **Impact:** Codec/container behavior now has a single source of truth; adding a codec/container requires one explicit registration path and duplicate/invalid registrations fail fast.
  - **Rollback:** Revert `src/capability_registry.*`, `src/main.cpp`, and `CMakeLists.txt` to the previous revision.
- `[2026-02-12] [Codex] [Media + Adapter Strong Registration]`
  - **What changed:** Added `src/adapter_registry.hpp` + `src/adapter_registry.cpp` with explicit media-kind and adapter registration (`input/output`, transport bindings, enabled flags), then routed request validation through this registry and added `/capabilities`.
  - **Why:** Extend strong-registration discipline beyond codec/container to media and transport adapter surfaces, preventing route-local drift.
  - **Impact:** `media=video|audio|av` is now validated from a single source of truth; unsupported media paths fail predictably; adapter inventory is machine-readable at runtime.
  - **Rollback:** Revert `src/adapter_registry.*`, `src/main.cpp`, `src/types.hpp`, and `CMakeLists.txt` to the previous revision.
- `[2026-02-12] [Codex] [WebSocket Sidecar Transport]`
  - **What changed:** Added `src/ws_server.*` websocket sidecar transport with RFC6455 upgrade handling and binary frame streaming (`mjpeg`/`h264`) on `--ws-port` (default `port+1`); wired shared lazy session attach/detach and warm/start logic through the same `SessionManager`; changed HTTP `/stream/ws` routes to return `426 upgrade_required` hints with `ws_url`; surfaced `ws_port`/`ws_enabled` in `/system/info`; enabled `websocket_stream` adapter when WS transport is compiled.
  - **Why:** Deliver working websocket transport without replacing the existing `cpp-httplib` HTTP stack, while preserving session correctness invariants.
  - **Impact:** Websocket clients can now connect to `ws://host:ws_port/stream/ws/{id}` and reuse existing session lifecycle semantics; API smoke checks now validate upgrade hints and sidecar listener behavior instead of placeholder `501`.
  - **Rollback:** Revert `src/ws_server.*`, `src/main.cpp`, `src/adapter_registry.cpp`, `CMakeLists.txt`, `scripts/smoke_api.sh`, `README.md`, and this `AGENTS.md` entry.
- `[2026-02-12] [Codex] [macOS Device Caps Introspection]`
  - **What changed:** Added AVFoundation capability snapshot builder (`build_avfoundation_caps_json`) in `src/capture_avfoundation.mm` with shared declarations in `src/capture_avfoundation.hpp`; wired `/device/{id}/caps` in `src/main.cpp` to return native format/fps capability JSON on macOS instead of Linux-only `caps_unavailable`; updated related docs.
  - **Why:** Remove API contract gap on macOS and keep discovery/debug tooling useful outside Linux-only V4L2 environments.
  - **Impact:** macOS now returns structured device capability data (active format + available formats/frame-rate ranges) for `/device/{id}/caps`; operators no longer hit unconditional Linux-only errors on supported macOS capture builds.
  - **Rollback:** Revert `src/capture_avfoundation.*`, `src/main.cpp`, `src/session_manager.cpp`, `CMakeLists.txt`, `README.md`, and this `AGENTS.md` entry.
- `[2026-02-12] [Codex] [Live/WS Conflict Softening]`
  - **What changed:** Removed strict `409 params locked by first requester` rejection on `/stream/live` and websocket attach paths for codec/container mismatches; route now serves the first-session effective codec while still honoring per-request H.264 container choice (`raw`/`mp4`) when compatible, and reports actual output via `Effective-Params`.
  - **Why:** Keep "Just GET" behavior resilient for follow-up clients instead of hard-failing on first-comer lock differences.
  - **Impact:** Requests like `codec=h264` then `codec=mjpeg` on the same active device no longer fail with `409`; clients receive the running effective stream profile and can inspect `Effective-Params` for actual codec/container.
  - **Rollback:** Revert `src/main.cpp`, `src/ws_server.cpp`, and this `AGENTS.md` entry.
- `[2026-03-06] [Codex] [RAW Live/WS Output]`
  - **What changed:** Added first-class `codec=raw` registration with `container=raw`; wired local capture to negotiate raw camera frames for RAW sessions; added HTTP live multipart raw responder and websocket binary raw responder with selectable `pixfmt=i420|rgb24`; surfaced RAW capability in `/capabilities` and README; explicitly rejected `rtsp+raw` and `udp+raw` until a decode/transport story exists.
  - **Why:** Expose the raw frame path SilkCast already had internally (`YUYV/NV12 -> I420`) without forcing clients through MJPEG or H.264 when they want inference-friendly bytes, while keeping transport shape stable for future raw formats.
  - **Impact:** `GET /stream/live/{id}?codec=raw&container=raw&pixfmt=i420|rgb24` now returns multipart raw frames, and `ws://.../stream/ws/{id}?codec=raw&container=raw&pixfmt=i420|rgb24` now sends one raw frame per WS binary message; current scope remains local capture sources only.
  - **Rollback:** Revert `src/capability_registry.cpp`, `src/capture_v4l2.cpp`, `src/main.cpp`, `src/stream_utils.*`, `src/ws_server.cpp`, `README.md`, and this `AGENTS.md` entry.

*(New agents: append entries; do not rewrite history.)*

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

### H. Codec and AV I/O Extensibility Reservations
* Reserve codec identifiers now: `mjpeg`, `h264`, `h265`, `av1`. Only enabled codecs can be negotiated at runtime.
* Keep `codec` negotiation separate from `container` negotiation so new codecs do not force API redesign.
* Use strong registration as the only mutation path: all codec/container capability changes must be registered in `src/capability_registry.*`, not hardcoded per-route.
* Apply the same strong registration rule to media and adapters: all media-kind and transport-adapter changes must be registered in `src/adapter_registry.*`.
* Treat media path adapters as first-class contracts:
  - `VideoInputAdapter` and `AudioInputAdapter`
  - `VideoOutputAdapter` and `AudioOutputAdapter` (including virtual device sinks)
* Plan Linux virtual-device targets explicitly: `v4l2loopback` for virtual camera and an audio loopback sink for virtual microphone routing.
* Apply the same session/refcount/reaper semantics to all media kinds (`video`, `audio`, `av`).

---

## 8. Implementation Notes (Current)
- Capture path uses V4L2; pixel format chosen by first requester: `codec=mjpeg` → MJPEG, `codec=h264/raw` → YUYV (converted to I420).
- RTSP input is supported via URL device IDs (`rtsp://...` encoded in path); RTSP sessions use H.264 pass-through where possible and reuse the same lazy session map/refcount cleanup semantics.
- H.264 encoding via optional OpenH264 (Cisco binary recommended for patent coverage); Annex-B NALs streamed over HTTP chunked.
- MJPEG and H.264 share lazy sessions; follow-up mismatched codec requests are served with first-session effective output where possible, and actual output is surfaced via `Effective-Params` instead of hard `409`.
- CLI flags: `--addr`, `--port`, `--ws-port`, `--idle-timeout`, `--codec`. Desktop launcher: `scripts/launch_desktop.sh` builds then opens the demo UI at `/` (override with env vars).
- Packaging: `scripts/build_linux.sh` for amd64/arm64; systemd unit at `packaging/systemd/silkcast.service`. macOS uses AVFoundation capture; other non-Linux/non-Apple builds still use stubs.
- I420 conversion is foundational; keep a fast YUYV→I420 path and avoid buffering (“latest frame only”) for preview/tele-op use. 
- Stats: `/stream/{id}/stats` returns fps/bitrate estimates plus lifecycle/debug fields (`state`, `teardown_reason`, `startup_ms`, `first_frame_ms`, `first_iframe_ms`) based on session counters and transitions; session tracks frames/bytes/clients and resets counters on first start. IDR forced on client join for H.264.
- Device caps: `/device/{id}/caps` returns native capability data on both Linux (V4L2) and macOS (AVFoundation), with platform-specific format metadata and frame-rate ranges.
- WebSocket: binary websocket streaming is available via sidecar listener (`ws://host:ws_port/stream/ws/{id}` or query form). HTTP `GET /stream/ws*` routes return `426 upgrade_required` with `ws_url` hints. Sidecar startup status is exposed via `/system/info` fields `ws_port` and `ws_enabled`.
- UDP: `/stream/udp/{id}?target=IP&port=5000&codec=h264&duration=10` sends fragmented packets with a custom binary header `[frame_id:4][frag_id:2][num_frags:2][data_size:4]` + payload. This enables robust reassembly and frame recovery on the client side.
- Feedback Loop: `POST /stream/{id}/feedback?type=idr` allows clients to request Instant Decoder Refresh (critical for H.264 packet loss recovery).
- Client SDK: `client/silkcast_client.py` provided as a reference Python implementation for high-performance receiving.
- fMP4: `/stream/live/{id}?codec=h264&container=mp4` returns chunked fragmented MP4 (tiny fragments, Baseline, IDR on join). CORS + no-store headers applied.
- OpenH264 fetch: Enabled by default. `AUTO_FETCH_OPENH264=ON` auto-downloads Cisco v2.6.0 binary+headers on Linux x86_64/arm64; else set `OPENH264_ROOT` or system install. Disable with `-DENABLE_OPENH264=OFF` if licensing blocks usage. Future codecs (H.265/AV1) are out-of-scope for now but keep build flags flexible.
- Current shipping focus is video stream delivery (MJPEG/H.264/RAW-I420/RGB24). Audio stream and virtual device paths are reserved in architecture and will follow the same lazy shared-session model when implemented.
- Codec negotiation now validates against `known={raw,mjpeg,h264,h265,av1}` and `enabled` at runtime; disabled but known codecs return structured `unsupported_codec` JSON.
- Capability registration source of truth: `src/capability_registry.*` defines known/enabled codecs, container compatibility, and per-transport container rules.
- Media/adapters registration source of truth: `src/adapter_registry.*` defines known/enabled media kinds and transport adapter availability; runtime snapshot is available at `GET /capabilities`.
