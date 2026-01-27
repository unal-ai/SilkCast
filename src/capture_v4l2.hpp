#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <mutex>

#include "types.hpp"

#ifdef __linux__
class CaptureV4L2 {
public:
  CaptureV4L2() = default;
  ~CaptureV4L2();

  bool start(const std::string &device_id, const CaptureParams &params);
  void stop();
  bool running() const { return running_; }
  bool latest_frame(std::string &out);
  PixelFormat pixel_format() const { return pixel_format_; }
  int width() const { return params_.width; }
  int height() const { return params_.height; }

private:
  void loop();
  bool configure_device(int fd, const CaptureParams &params);

  std::string device_id_;
  CaptureParams params_;
  PixelFormat pixel_format_ = PixelFormat::UNKNOWN;
  int fd_ = -1;
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::string buffer_; // latest frame (MJPEG)
  std::mutex buf_mu_;
};
#else
// Non-Linux stub to keep buildable on macOS/Windows during development.
class CaptureV4L2 {
public:
  bool start(const std::string &, const CaptureParams &) { return false; }
  void stop() {}
  bool running() const { return false; }
  bool latest_frame(std::string &) { return false; }
};
#endif
