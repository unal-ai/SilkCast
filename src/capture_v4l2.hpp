#pragma once

#include "capture_interface.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef __linux__
class CaptureV4L2 : public CaptureInterface {
public:
  CaptureV4L2() = default;
  ~CaptureV4L2() override;

  bool start(const std::string &device_id,
             const CaptureParams &params) override;
  void stop() override;
  bool running() const override { return running_; }
  bool latest_frame(std::string &out) override;
  uint64_t latest_frame_sequence() const override {
    return frame_sequence_.load(std::memory_order_relaxed);
  }
  PixelFormat pixel_format() const override { return pixel_format_; }
  int width() const override { return params_.width; }
  int height() const override { return params_.height; }
  int fps() const override { return params_.fps; }
  void get_sps_pps(std::vector<uint8_t> &sps,
                   std::vector<uint8_t> &pps) override {
    sps.clear();
    pps.clear();
  }

private:
  void loop();
  void loop_mmap();
  void loop_read();
  bool configure_device(int fd, CaptureParams &params);
  void cleanup_mmap_setup_failure(int fd);
  void cleanup_mmap_buffers();

  std::string device_id_;
  CaptureParams params_;
  PixelFormat pixel_format_ = PixelFormat::UNKNOWN;
  int fd_ = -1;
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frame_sequence_{0};
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
class CaptureV4L2 : public CaptureInterface {
public:
  CaptureV4L2();
  ~CaptureV4L2() override;

  bool start(const std::string &device_id,
             const CaptureParams &params) override;
  void stop() override;
  bool running() const override { return running_; }
  bool latest_frame(std::string &out) override;
  uint64_t latest_frame_sequence() const override {
    return frame_sequence_.load(std::memory_order_relaxed);
  }
  PixelFormat pixel_format() const override { return pixel_format_; }
  int width() const override { return params_.width; }
  int height() const override { return params_.height; }
  int fps() const override { return params_.fps; }
  void get_sps_pps(std::vector<uint8_t> &sps,
                   std::vector<uint8_t> &pps) override {
    sps.clear();
    pps.clear();
  }
  void handle_sample(void *sample_buffer);

private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
  std::string device_id_;
  CaptureParams params_;
  PixelFormat pixel_format_ = PixelFormat::UNKNOWN;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frame_sequence_{0};
  std::string buffer_;
  std::mutex buf_mu_;
};
#else
// Non-Linux stub to keep buildable on macOS/Windows during development.
class CaptureV4L2 : public CaptureInterface {
public:
  bool start(const std::string &, const CaptureParams &) override {
    return false;
  }
  void stop() override {}
  bool running() const override { return false; }
  bool latest_frame(std::string &) override { return false; }
  uint64_t latest_frame_sequence() const override { return 0; }
  PixelFormat pixel_format() const override { return PixelFormat::UNKNOWN; }
  int width() const override { return 0; }
  int height() const override { return 0; }
  int fps() const override { return 0; }
  void get_sps_pps(std::vector<uint8_t> &sps,
                   std::vector<uint8_t> &pps) override {
    sps.clear();
    pps.clear();
  }
};
#endif
