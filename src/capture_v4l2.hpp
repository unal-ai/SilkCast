#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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
  void loop_mmap();
  void loop_read();
  bool configure_device(int fd, const CaptureParams &params);
  void cleanup_mmap_setup_failure(int fd);
  void cleanup_mmap_buffers();

  std::string device_id_;
  CaptureParams params_;
  PixelFormat pixel_format_ = PixelFormat::UNKNOWN;
  int fd_ = -1;
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::string buffer_; // latest frame
  std::mutex buf_mu_;

  // mmap streaming support
  bool use_mmap_ = false;
  static constexpr unsigned kNumBuffers = 4;
  struct MmapBuffer {
    void *start = nullptr;
    size_t length = 0;
  };
  MmapBuffer buffers_[kNumBuffers];
  unsigned num_buffers_ = 0;
  size_t frame_size_ = 0;
};
#elif defined(__APPLE__)
class CaptureV4L2 {
public:
  CaptureV4L2();
  ~CaptureV4L2();

  bool start(const std::string &device_id, const CaptureParams &params);
  void stop();
  bool running() const { return running_; }
  bool latest_frame(std::string &out);
  PixelFormat pixel_format() const { return pixel_format_; }
  int width() const { return params_.width; }
  int height() const { return params_.height; }
  void handle_sample(void *sample_buffer);

private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
  std::string device_id_;
  CaptureParams params_;
  PixelFormat pixel_format_ = PixelFormat::UNKNOWN;
  std::atomic<bool> running_{false};
  std::string buffer_;
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
  PixelFormat pixel_format() const { return PixelFormat::UNKNOWN; }
};
#endif
