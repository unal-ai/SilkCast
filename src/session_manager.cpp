#include "session_manager.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

#ifdef __APPLE__
std::vector<std::string> list_avfoundation_devices();
#endif
#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

SessionManager::SessionManager(int idle_timeout_seconds)
    : idle_timeout_seconds_(idle_timeout_seconds),
      reaper_thread_([this] { reap_loop(); }) {}

SessionManager::~SessionManager() {
  stop_reaper_ = true;
  if (reaper_thread_.joinable())
    reaper_thread_.join();
}

std::shared_ptr<Session>
SessionManager::get_or_create(const std::string &device_id,
                              const CaptureParams &params) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(device_id);
  if (it != sessions_.end()) {
    return it->second;
  }
  auto session = std::make_shared<Session>();
  session->device_id = device_id;
  session->params = params;
  session->capture = std::make_shared<CaptureV4L2>();
  sessions_[device_id] = session;
  return session;
}

void SessionManager::touch(const std::string &device_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(device_id);
  if (it != sessions_.end()) {
    it->second->last_accessed = std::chrono::steady_clock::now();
  }
}

void SessionManager::release_if_idle(const std::string &device_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(device_id);
  if (it != sessions_.end()) {
    if (it->second->client_count.load() == 0) {
      if (it->second->capture)
        it->second->capture->stop();
      sessions_.erase(it);
    }
  }
}

std::vector<std::string> SessionManager::list_devices() const {
  std::vector<std::string> devices;
#ifdef __APPLE__
  devices = list_avfoundation_devices();
#else
  for (const auto &entry : std::filesystem::directory_iterator("/dev")) {
    const auto name = entry.path().filename().string();
    if (name.rfind("video", 0) == 0) {
#ifdef __linux__
      int fd = open(entry.path().c_str(), O_RDWR | O_NONBLOCK, 0);
      if (fd >= 0) {
        v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
          __u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                           ? cap.device_caps
                           : cap.capabilities;
          if (caps & V4L2_CAP_VIDEO_CAPTURE) {
            devices.push_back(name);
          }
        }
        close(fd);
      }
#else
      devices.push_back(name);
#endif
    }
  }
#endif
  if (devices.empty()) {
    devices.push_back("video0"); // fallback hint
  }
  std::sort(devices.begin(), devices.end());
  return devices;
}

std::optional<std::shared_ptr<Session>>
SessionManager::find(const std::string &device_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(device_id);
  if (it == sessions_.end())
    return std::nullopt;
  return it->second;
}

void SessionManager::reap_loop() {
  while (!stop_reaper_) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto now = std::chrono::steady_clock::now();
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        auto &sess = it->second;
        auto idle_for = std::chrono::duration_cast<std::chrono::seconds>(
                            now - sess->last_accessed)
                            .count();
        if (sess->client_count.load() == 0 &&
            idle_for > idle_timeout_seconds_) {
          if (sess->capture)
            sess->capture->stop();
          it = sessions_.erase(it);
        } else {
          ++it;
        }
      }
    }
    std::this_thread::sleep_for(10s);
  }
}
