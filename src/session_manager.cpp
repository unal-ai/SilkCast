#include "session_manager.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

#include "capture_avfoundation.hpp"
#include "capture_rtsp.hpp"
#include "stream_utils.hpp"
#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

bool same_source_params(const CaptureParams &a, const CaptureParams &b) {
  if (a.width != b.width || a.height != b.height || a.fps != b.fps ||
      a.media != b.media || a.codec != b.codec) {
    return false;
  }
  if (a.codec == "mjpeg") {
    return a.quality == b.quality;
  }
  return true;
}

std::string normalize_local_device_path(const std::string &device_id) {
  if (device_id.rfind("rtsp://", 0) == 0) {
    return device_id;
  }
  if (device_id.rfind("/dev/", 0) == 0) {
    return device_id;
  }
  if (device_id.rfind("video", 0) == 0) {
    return "/dev/" + device_id;
  }
  return device_id;
}

std::string canonical_session_key(const std::string &device_id) {
  const auto normalized = normalize_local_device_path(device_id);
  if (normalized.rfind("/dev/", 0) != 0) {
    return normalized;
  }
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(normalized, ec);
  if (!ec) {
    return canonical.string();
  }
  return normalized;
}

std::shared_ptr<Session> make_session(const std::string &device_id,
                                      const CaptureParams &params) {
  auto session = std::make_shared<Session>();
  session->device_id = device_id;
  session->params = params;
  if (device_id.rfind("rtsp://", 0) == 0) {
    session->capture = std::make_shared<CaptureRTSP>();
  } else {
    session->capture = std::make_shared<CaptureV4L2>();
  }
  return session;
}

} // namespace

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
  const auto session_key = canonical_session_key(device_id);
  const CaptureParams source_params =
      stream::normalize_source_params(device_id, params);
  auto it = sessions_.find(session_key);
  if (it != sessions_.end()) {
    auto &existing = it->second;
    const bool no_clients = existing->client_count.load() == 0;
    const bool stale_capture =
        !existing->capture || !existing->capture->running();
    if ((!same_source_params(existing->params, source_params) || stale_capture) &&
        no_clients) {
      if (existing->capture) {
        existing->capture->stop();
      }
      existing->state.store(SessionState::Idle);
      existing->teardown_reason.store(
          stale_capture ? TeardownReason::OpenFailed
                        : TeardownReason::IdleTimeout);
      it->second = make_session(session_key, source_params);
      return it->second;
    }
    return it->second;
  }
  auto session = make_session(session_key, source_params);
  sessions_[session_key] = session;
  return session;
}

void SessionManager::touch(const std::string &device_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(canonical_session_key(device_id));
  if (it != sessions_.end()) {
    it->second->last_accessed = std::chrono::steady_clock::now();
  }
}

void SessionManager::release_if_idle(const std::string &device_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(canonical_session_key(device_id));
  if (it != sessions_.end()) {
    if (it->second->client_count.load() == 0) {
      if (it->second->capture && it->second->capture->running()) {
        it->second->state.store(SessionState::Draining);
      }
      // Preserve short-gap session reuse; let the reaper own teardown timing.
      it->second->last_accessed = std::chrono::steady_clock::now();
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
  auto it = sessions_.find(canonical_session_key(device_id));
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
          if (sess->capture) {
            sess->capture->stop();
          }
          sess->state.store(SessionState::Idle);
          sess->teardown_reason.store(TeardownReason::IdleTimeout);
          std::cout << "[reaper] device=" << sess->device_id
                    << " state=idle reason=idle_timeout idle_for_s=" << idle_for
                    << std::endl;
          it = sessions_.erase(it);
        } else {
          ++it;
        }
      }
    }
    std::this_thread::sleep_for(10s);
  }
}
