#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture_interface.hpp"

class CaptureRTSP : public CaptureInterface {
public:
  ~CaptureRTSP() override;

  bool start(const std::string &url, const CaptureParams &params) override;
  void stop() override;
  bool latest_frame(std::string &out) override;
  uint64_t latest_frame_sequence() const override {
    return frame_sequence_.load(std::memory_order_relaxed);
  }
  bool running() const override { return running_; }
  int width() const override { return params_.width; }
  int height() const override { return params_.height; }
  int fps() const override { return params_.fps; }
  PixelFormat pixel_format() const override { return PixelFormat::H264; }
  void get_sps_pps(std::vector<uint8_t> &sps,
                   std::vector<uint8_t> &pps) override;

private:
  void loop();
  bool connect_rtsp();
  void send_keepalive();

  int send_cmd(int sock, const std::string &cmd);
  bool read_line(int sock, std::string &line);
  bool read_response(int sock, std::string &response);
  bool parse_sdp(const std::string &sdp, std::string &control_url);
  bool parse_url(const std::string &url, std::string &host, int &port,
                 std::string &path);

  std::string url_;
  std::string session_id_;
  std::string control_url_;
  CaptureParams params_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_flag_{false};
  std::atomic<uint64_t> frame_sequence_{0};
  std::thread thread_;

  mutable std::mutex buf_mu_;
  std::string buffer_;

  mutable std::mutex meta_mu_;
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;

  int sock_ = -1;
  std::chrono::steady_clock::time_point last_keepalive_{
      std::chrono::steady_clock::now()};
};
