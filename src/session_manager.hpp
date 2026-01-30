#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "capture_v4l2.hpp"
#include "types.hpp"

class SessionManager {
public:
  explicit SessionManager(int idle_timeout_seconds);
  ~SessionManager();

  std::shared_ptr<Session> get_or_create(const std::string &device_id,
                                         const CaptureParams &params);
  void touch(const std::string &device_id);
  void release_if_idle(const std::string &device_id);
  std::vector<std::string> list_devices() const;
  std::optional<std::shared_ptr<Session>> find(const std::string &device_id);

private:
  void reap_loop();

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
  std::thread reaper_thread_;
  std::atomic<bool> stop_reaper_{false};
  const int idle_timeout_seconds_;
};
