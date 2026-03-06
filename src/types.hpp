#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct CaptureParams {
  int width = 640;
  int height = 480;
  int fps = 15;
  int bitrate_kbps = 256;
  int quality = 80; // JPEG quality (1-100) for MJPEG
  int gop = 30;
  std::string media = "video";   // video | audio | av
  std::string codec = "mjpeg";   // h264 | mjpeg | raw | h265 | av1 (reserved)
  std::string pixfmt = "";       // i420 | rgb24 (codec=raw only)
  std::string latency = "view";  // view | low | ultra
  std::string container = "raw"; // raw | mp4 (fMP4)
};

enum class PixelFormat { MJPEG, YUYV, NV12, H264, UNKNOWN };

enum class SessionState { Idle, Warming, Live, Draining };

enum class TeardownReason { None, IdleTimeout, OpenFailed, RuntimeError };

inline const char *session_state_label(SessionState state) {
  switch (state) {
  case SessionState::Idle:
    return "idle";
  case SessionState::Warming:
    return "warming";
  case SessionState::Live:
    return "live";
  case SessionState::Draining:
    return "draining";
  }
  return "unknown";
}

inline const char *teardown_reason_label(TeardownReason reason) {
  switch (reason) {
  case TeardownReason::None:
    return "none";
  case TeardownReason::IdleTimeout:
    return "idle_timeout";
  case TeardownReason::OpenFailed:
    return "open_failed";
  case TeardownReason::RuntimeError:
    return "runtime_error";
  }
  return "unknown";
}

struct EffectiveParams {
  CaptureParams requested;
  CaptureParams actual;
};

struct Session {
  std::string device_id;
  CaptureParams params;
  std::shared_ptr<class CaptureInterface> capture;
  std::shared_ptr<class H264Encoder> encoder;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  uint32_t seqno = 1;
  PixelFormat pixel_format = PixelFormat::UNKNOWN;
  std::atomic<int> client_count{0};
  std::atomic<bool> running{false};
  std::atomic<SessionState> state{SessionState::Idle};
  std::atomic<TeardownReason> teardown_reason{TeardownReason::None};
  std::chrono::steady_clock::time_point last_accessed =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  std::atomic<uint64_t> frames_sent{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> startup_ms{0};
  std::atomic<uint64_t> first_frame_ms{0};
  std::atomic<uint64_t> first_iframe_ms{0};
  std::atomic<bool> first_frame_marked{false};
  std::atomic<bool> first_iframe_marked{false};
  std::atomic<uint32_t> idr_request_seq{0};
};

#pragma pack(push, 1)
struct UdpFrameHeader {
  uint32_t frame_id;
  uint16_t frag_id;
  uint16_t num_frags;
  uint32_t data_size; // Payload size in this packet
};
#pragma pack(pop)
