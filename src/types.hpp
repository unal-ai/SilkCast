#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

struct CaptureParams {
  int width = 640;
  int height = 480;
  int fps = 15;
  int bitrate_kbps = 256;
  int gop = 30;
  std::string codec = "mjpeg";  // "h264" or "mjpeg"
  std::string latency = "view"; // view | low | ultra
  std::string container = "raw"; // raw | mp4 (fMP4)
};

enum class PixelFormat {
  MJPEG,
  YUYV,
  UNKNOWN
};

struct EffectiveParams {
  CaptureParams requested;
  CaptureParams actual;
};

struct Session {
  std::string device_id;
  CaptureParams params;
  std::shared_ptr<class CaptureV4L2> capture;
  std::shared_ptr<class H264Encoder> encoder;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  uint32_t seqno = 1;
  PixelFormat pixel_format = PixelFormat::UNKNOWN;
  std::atomic<int> client_count{0};
  std::atomic<bool> running{false};
  std::chrono::steady_clock::time_point last_accessed = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::atomic<uint64_t> frames_sent{0};
  std::atomic<uint64_t> bytes_sent{0};
};
