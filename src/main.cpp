#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "api_router.hpp"
#include "capture_v4l2.hpp"
#include "encoder_h264.hpp"
#include "httplib.h"
#include "index_html.hpp"
#include "mp4_frag.hpp"
#include "types.hpp"
#include "yuv_convert.hpp"

#ifdef __APPLE__
std::vector<std::string> list_avfoundation_devices();
#endif
#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;

class SessionManager {
public:
  explicit SessionManager(int idle_timeout_seconds)
      : idle_timeout_seconds_(idle_timeout_seconds),
        reaper_thread_([this] { reap_loop(); }) {}
  ~SessionManager() {
    stop_reaper_ = true;
    if (reaper_thread_.joinable())
      reaper_thread_.join();
  }

  std::shared_ptr<Session> get_or_create(const std::string &device_id,
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

  void touch(const std::string &device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(device_id);
    if (it != sessions_.end()) {
      it->second->last_accessed = std::chrono::steady_clock::now();
    }
  }

  void release_if_idle(const std::string &device_id) {
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

  std::vector<std::string> list_devices() const {
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

  std::optional<std::shared_ptr<Session>> find(const std::string &device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(device_id);
    if (it == sessions_.end())
      return std::nullopt;
    return it->second;
  }

private:
  void reap_loop() {
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

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
  std::thread reaper_thread_;
  std::atomic<bool> stop_reaper_{false};
  const int idle_timeout_seconds_;
};

// Minimal 1x1 white JPEG (valid) for placeholder MJPEG stream.
static const unsigned char kTinyJpeg[] = {
    0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02,
    0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05,
    0x05, 0x04, 0x04, 0x05, 0x0A, 0x07, 0x07, 0x06, 0x08, 0x0C, 0x0A, 0x0C,
    0x0C, 0x0B, 0x0A, 0x0B, 0x0B, 0x0D, 0x0E, 0x12, 0x10, 0x0D, 0x0E, 0x11,
    0x0E, 0x0B, 0x0B, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15,
    0x0C, 0x0F, 0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xFF,
    0xC0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x01, 0x03, 0x01, 0x11, 0x00,
    0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03,
    0x11, 0x00, 0x3F, 0x00, 0xFF, 0xD9};

std::string json_array(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += "\"" + items[i] + "\"";
    if (i + 1 < items.size())
      out += ",";
  }
  out += "]";
  return out;
}

std::string build_error_json(const std::string &msg,
                             const std::string &details = "") {
  std::string out = "{\"error\":\"" + msg + "\"";
  if (!details.empty())
    out += ",\"details\":\"" + details + "\"";
  out += "}";
  return out;
}

const char *pixel_format_label(PixelFormat fmt) {
  switch (fmt) {
  case PixelFormat::MJPEG:
    return "mjpeg";
  case PixelFormat::YUYV:
    return "yuyv";
  case PixelFormat::NV12:
    return "nv12";
  default:
    return "unknown";
  }
}

#ifdef __linux__
bool xioctl_device(int fd, unsigned long request, void *arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r != -1;
}

std::string fourcc_to_string(__u32 fmt) {
  char fourcc[5] = {static_cast<char>(fmt & 0xFF),
                    static_cast<char>((fmt >> 8) & 0xFF),
                    static_cast<char>((fmt >> 16) & 0xFF),
                    static_cast<char>((fmt >> 24) & 0xFF), 0};
  return std::string(fourcc);
}

std::string build_device_caps_json(const std::string &device_id,
                                   std::string &error) {
  std::string dev_path =
      device_id.rfind("/dev/", 0) == 0 ? device_id : "/dev/" + device_id;
  int fd = open(dev_path.c_str(), O_RDWR | O_NONBLOCK, 0);
  if (fd < 0) {
    error = "failed to open device";
    return "";
  }

  v4l2_capability cap{};
  if (!xioctl_device(fd, VIDIOC_QUERYCAP, &cap)) {
    error = "VIDIOC_QUERYCAP failed";
    close(fd);
    return "";
  }
  __u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                         : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
    error = "device does not support video capture";
    close(fd);
    return "";
  }

  std::ostringstream ss;
  ss << "{";
  ss << "\"device\":\"" << json_escape(device_id) << "\",";
  ss << "\"card\":\"" << json_escape(reinterpret_cast<char *>(cap.card))
     << "\",";
  ss << "\"driver\":\"" << json_escape(reinterpret_cast<char *>(cap.driver))
     << "\",";
  ss << "\"bus_info\":\"" << json_escape(reinterpret_cast<char *>(cap.bus_info))
     << "\"";

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl_device(fd, VIDIOC_G_FMT, &fmt)) {
    ss << ",\"current\":{";
    ss << "\"width\":" << fmt.fmt.pix.width << ",";
    ss << "\"height\":" << fmt.fmt.pix.height << ",";
    ss << "\"fourcc\":\"" << fourcc_to_string(fmt.fmt.pix.pixelformat) << "\"";
    v4l2_streamparm sp{};
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl_device(fd, VIDIOC_G_PARM, &sp)) {
      const auto num = sp.parm.capture.timeperframe.numerator;
      const auto den = sp.parm.capture.timeperframe.denominator;
      if (num > 0 && den > 0) {
        ss << ",\"fps\":" << (den / num);
      }
    }
    ss << "}";
  }

  ss << ",\"formats\":[";
  bool first_format = true;
  v4l2_fmtdesc fdesc{};
  fdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (fdesc.index = 0; xioctl_device(fd, VIDIOC_ENUM_FMT, &fdesc);
       ++fdesc.index) {
    if (!first_format)
      ss << ",";
    first_format = false;
    ss << "{";
    ss << "\"fourcc\":\"" << fourcc_to_string(fdesc.pixelformat) << "\",";
    ss << "\"description\":\""
       << json_escape(reinterpret_cast<char *>(fdesc.description)) << "\",";
    ss << "\"sizes\":[";

    bool first_size = true;
    v4l2_frmsizeenum fsize{};
    fsize.pixel_format = fdesc.pixelformat;
    for (fsize.index = 0;
         xioctl_device(fd, VIDIOC_ENUM_FRAMESIZES, &fsize); ++fsize.index) {
      if (!first_size)
        ss << ",";
      first_size = false;

      if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        const int w = static_cast<int>(fsize.discrete.width);
        const int h = static_cast<int>(fsize.discrete.height);
        ss << "{";
        ss << "\"type\":\"discrete\",";
        ss << "\"width\":" << w << ",";
        ss << "\"height\":" << h;
        ss << ",\"intervals\":[";

        bool first_interval = true;
        v4l2_frmivalenum ival{};
        ival.pixel_format = fdesc.pixelformat;
        ival.width = fsize.discrete.width;
        ival.height = fsize.discrete.height;
        for (ival.index = 0;
             xioctl_device(fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival);
             ++ival.index) {
          if (!first_interval)
            ss << ",";
          first_interval = false;
          if (ival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            ss << "{\"numerator\":" << ival.discrete.numerator
               << ",\"denominator\":" << ival.discrete.denominator << "}";
          } else {
            ss << "{\"type\":\"stepwise\",\"min\":{\"numerator\":"
               << ival.stepwise.min.numerator
               << ",\"denominator\":" << ival.stepwise.min.denominator
               << "},\"max\":{\"numerator\":" << ival.stepwise.max.numerator
               << ",\"denominator\":" << ival.stepwise.max.denominator
               << "},\"step\":{\"numerator\":" << ival.stepwise.step.numerator
               << ",\"denominator\":" << ival.stepwise.step.denominator
               << "}}";
            break;
          }
        }
        ss << "]";
        ss << "}";
      } else {
        const auto &step = fsize.stepwise;
        ss << "{\"type\":\"stepwise\",";
        ss << "\"min_width\":" << step.min_width << ",";
        ss << "\"max_width\":" << step.max_width << ",";
        ss << "\"step_width\":" << step.step_width << ",";
        ss << "\"min_height\":" << step.min_height << ",";
        ss << "\"max_height\":" << step.max_height << ",";
        ss << "\"step_height\":" << step.step_height << "}";
      }
    }

    ss << "]";
    ss << "}";
  }
  ss << "]";
  ss << "}";

  close(fd);
  return ss.str();
}
#endif

// Convert Annex-B NAL stream to length-prefixed (AVCC) single-sample buffer.
std::vector<uint8_t> annexb_to_avcc(const std::string &annexb) {
  std::vector<uint8_t> out;
  size_t i = 0;
  auto len = annexb.size();
  auto is_start_code = [&annexb, len](size_t pos) {
    return pos + 2 < len && annexb[pos] == 0 && annexb[pos + 1] == 0 &&
           (annexb[pos + 2] == 1 ||
            (annexb[pos + 2] == 0 && pos + 3 < len && annexb[pos + 3] == 1));
  };
  while (i + 3 < len) {
    if (!is_start_code(i)) {
      ++i;
      continue;
    }
    size_t sc_size = (annexb[i + 2] == 1) ? 3 : 4;
    size_t start = i + sc_size;
    size_t next = start;
    while (next + 3 < len && !is_start_code(next)) {
      ++next;
    }
    size_t end = (next + 3 < len) ? next : len;
    size_t nalsize = end - start;
    uint32_t n = static_cast<uint32_t>(nalsize);
    out.push_back((n >> 24) & 0xFF);
    out.push_back((n >> 16) & 0xFF);
    out.push_back((n >> 8) & 0xFF);
    out.push_back(n & 0xFF);
    out.insert(out.end(), annexb.begin() + start,
               annexb.begin() + start + nalsize);
    i = next;
  }
  return out;
}

// Extract SPS/PPS from Annex-B sample (first IDR)
void extract_sps_pps(const std::string &annexb, std::vector<uint8_t> &sps,
                     std::vector<uint8_t> &pps) {
  size_t i = 0;
  auto len = annexb.size();
  auto is_start_code = [&annexb, len](size_t pos) {
    return pos + 2 < len && annexb[pos] == 0 && annexb[pos + 1] == 0 &&
           (annexb[pos + 2] == 1 ||
            (annexb[pos + 2] == 0 && pos + 3 < len && annexb[pos + 3] == 1));
  };
  while (i + 3 < len) {
    if (!is_start_code(i)) {
      ++i;
      continue;
    }
    size_t sc_size = (annexb[i + 2] == 1) ? 3 : 4;
    size_t start = i + sc_size;
    size_t next = start;
    while (next + 3 < len && !is_start_code(next)) {
      ++next;
    }
    size_t end = (next + 3 < len) ? next : len;
    size_t nalsize = end - start;
    if (nalsize == 0) {
      i = next;
      continue;
    }
    uint8_t nal_type = static_cast<uint8_t>(annexb[start]) & 0x1F;
    if (nal_type == 7 && sps.empty()) {
      sps.assign(annexb.begin() + start, annexb.begin() + start + nalsize);
    } else if (nal_type == 8 && pps.empty()) {
      pps.assign(annexb.begin() + start, annexb.begin() + start + nalsize);
    }
    if (!sps.empty() && !pps.empty())
      break;
    i = next;
  }
}

CaptureParams parse_params(const httplib::Request &req) {
  CaptureParams p;
  if (req.has_param("w"))
    p.width = std::stoi(req.get_param_value("w"));
  if (req.has_param("h"))
    p.height = std::stoi(req.get_param_value("h"));
  if (req.has_param("fps"))
    p.fps = std::stoi(req.get_param_value("fps"));
  if (req.has_param("bitrate"))
    p.bitrate_kbps = std::stoi(req.get_param_value("bitrate"));
  if (req.has_param("gop"))
    p.gop = std::stoi(req.get_param_value("gop"));
  if (req.has_param("codec"))
    p.codec = req.get_param_value("codec");
  if (req.has_param("latency"))
    p.latency = req.get_param_value("latency");
  if (req.has_param("container"))
    p.container = req.get_param_value("container");
  return p;
}

void sync_session_params(Session &session) {
  if (!session.capture)
    return;
  // Devices may clamp to supported modes; keep session params in sync.
  const int w = session.capture->width();
  const int h = session.capture->height();
  const int fps = session.capture->fps();
  if (w > 0)
    session.params.width = w;
  if (h > 0)
    session.params.height = h;
  if (fps > 0)
    session.params.fps = fps;
  session.pixel_format = session.capture->pixel_format();
}

void add_effective_headers(httplib::Response &res, const EffectiveParams &eff) {
  const auto &a = eff.actual;
  res.set_header("Effective-Params",
                 "codec=" + a.codec + ";w=" + std::to_string(a.width) +
                     ";h=" + std::to_string(a.height) +
                     ";fps=" + std::to_string(a.fps) +
                     ";bitrate=" + std::to_string(a.bitrate_kbps) +
                     ";gop=" + std::to_string(a.gop) + ";latency=" + a.latency +
                     ";container=" + a.container);
}

void serve_mjpeg_placeholder(const CaptureParams &p, httplib::Response &res,
                             std::shared_ptr<Session> session,
                             std::function<void(bool)> on_done) {
  const auto boundary = "frame";
  res.set_header("Connection", "close");
  res.set_chunked_content_provider(
      "multipart/x-mixed-replace; boundary=" + std::string(boundary),
      [p, boundary, session](size_t, httplib::DataSink &sink) mutable {
        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        std::string prefix =
            "--" + std::string(boundary) +
            "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
            std::to_string(sizeof(kTinyJpeg)) + "\r\n\r\n";
        for (;;) {
          if (!sink.write(prefix.data(), prefix.size()))
            return false;
          if (!sink.write(reinterpret_cast<const char *>(kTinyJpeg),
                          sizeof(kTinyJpeg)))
            return false;
          if (!sink.write("\r\n", 2))
            return false;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(prefix.size() + sizeof(kTinyJpeg) + 2);
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
}

void serve_mjpeg_live(const CaptureParams &p, httplib::Response &res,
                      std::shared_ptr<Session> session,
                      std::function<void(bool)> on_done) {
  const auto boundary = "frame";
  res.set_header("Connection", "close");
  res.set_chunked_content_provider(
      "multipart/x-mixed-replace; boundary=" + std::string(boundary),
      [p, boundary, session](size_t, httplib::DataSink &sink) mutable {
        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        std::string prefix;
        std::string frame;
        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          if (session->capture->pixel_format() != PixelFormat::MJPEG ||
              !session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          prefix = "--" + std::string(boundary) +
                   "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                   std::to_string(frame.size()) + "\r\n\r\n";
          if (!sink.write(prefix.data(), prefix.size()))
            return false;
          if (!sink.write(frame.data(), frame.size()))
            return false;
          if (!sink.write("\r\n", 2))
            return false;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(prefix.size() + frame.size() + 2);
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
}

void serve_h264_live(const CaptureParams &p, httplib::Response &res,
                     std::shared_ptr<Session> session,
                     std::function<void(bool)> on_done) {
#ifdef HAS_OPENH264
  res.set_header("Connection", "close");
  res.set_header("Content-Type", "video/H264");
  res.set_chunked_content_provider(
      "video/H264",
      [p, session](size_t, httplib::DataSink &sink) mutable {
        H264Encoder encoder;
        bool encoder_ready = false;
        if (!encoder_ready) {
          if (!encoder.init(session->params)) {
            return false;
          }
          encoder.force_idr();
          encoder_ready = true;
        }
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        yuv.resize(y_size + 2 * uv_size);
        uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;

        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        bool first = true;
        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          PixelFormat fmt = session->capture->pixel_format();
          if ((fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) ||
              !session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          if (fmt == PixelFormat::YUYV) {
            yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                         p.width, p.height, y, u, v);
          } else {
            const uint8_t *src_y =
                reinterpret_cast<const uint8_t *>(frame.data());
            const uint8_t *src_uv = src_y + (p.width * p.height);
            nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width, y,
                         u, v);
          }
          if (first) {
            encoder.force_idr();
            first = false;
          }
          std::string nal;
          if (!encoder.encode_i420(y, u, v, nal)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (!nal.empty()) {
            // Annex B start code 00 00 00 01
            static const char start_code[] = {0, 0, 0, 1};
            if (!sink.write(start_code, 4))
              return false;
            if (!sink.write(nal.data(), nal.size()))
              return false;
            session->frames_sent.fetch_add(1);
            session->bytes_sent.fetch_add(4 + nal.size());
            session->last_accessed = std::chrono::steady_clock::now();
          }
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
#else
  (void)p;
  // (void)session;
  res.status = 503;
  res.set_content(build_error_json("h264_unavailable", "OpenH264 not enabled"),
                  "application/json");
  on_done(false);
#endif
}

void serve_fmp4_live(const CaptureParams &p, httplib::Response &res,
                     std::shared_ptr<Session> session,
                     std::function<void(bool)> on_done) {
#ifdef HAS_OPENH264
  res.set_header("Connection", "close");
  res.set_header("Content-Type", "video/mp4");
  res.set_header("Cache-Control", "no-store");
  res.set_header("Access-Control-Allow-Origin", "*");
  const uint32_t sample_duration = p.fps > 0 ? (90000 / p.fps) : 6000;

  res.set_chunked_content_provider(
      "video/mp4",
      [p, session, sample_duration](size_t, httplib::DataSink &sink) mutable {
        H264Encoder encoder;
        if (!encoder.init(session->params))
          return false;
        encoder.force_idr();
        std::vector<uint8_t> sps = session->sps;
        std::vector<uint8_t> pps = session->pps;
        uint32_t seqno = 1;
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        yuv.resize(y_size + 2 * uv_size);
        uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;
        bool sent_init = false;
        Mp4Fragmenter *mux = nullptr;
        std::unique_ptr<Mp4Fragmenter> mux_guard;
        uint64_t decode_time = 0;
        if (!sps.empty() && !pps.empty()) {
          mux_guard = std::make_unique<Mp4Fragmenter>(p.width, p.height, p.fps,
                                                      sps, pps);
          mux = mux_guard.get();
        }
        while (true) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          if (!session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          PixelFormat fmt = session->capture->pixel_format();
          if (fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (fmt == PixelFormat::YUYV) {
            yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                         p.width, p.height, y, u, v);
          } else {
            const uint8_t *src_y =
                reinterpret_cast<const uint8_t *>(frame.data());
            const uint8_t *src_uv = src_y + (p.width * p.height);
            nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width, y,
                         u, v);
          }
          std::string nal_annexb;
          if (!encoder.encode_i420(y, u, v, nal_annexb)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (sps.empty() || pps.empty()) {
            extract_sps_pps(nal_annexb, sps, pps);
            if (!sps.empty() && !pps.empty()) {
              session->sps = sps;
              session->pps = pps;
              mux_guard = std::make_unique<Mp4Fragmenter>(p.width, p.height,
                                                          p.fps, sps, pps);
              mux = mux_guard.get();
            }
          }
          if (!mux) {
            continue;
          }
          if (!sent_init) {
            auto init_seg = mux->build_init_segment();
            if (!sink.write(init_seg.data(), init_seg.size()))
              return false;
            sent_init = true;
          }
          auto avcc = annexb_to_avcc(nal_annexb);
          bool keyframe = !nal_annexb.empty() && ((nal_annexb[4] & 0x1F) == 5);
          auto frag = mux->build_fragment(avcc, seqno++, decode_time,
                                          sample_duration, keyframe);
          decode_time += sample_duration;
          if (!sink.write(frag.data(), frag.size()))
            return false;

          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(frag.size());
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(
              std::chrono::milliseconds(1000 / std::max(1, p.fps)));
        }
        return true;
      },
      on_done);
#else
  (void)p;
  // (void)session;
  res.status = 503;
  res.set_content(build_error_json("h264_unavailable", "OpenH264 not enabled"),
                  "application/json");
  on_done(false);
#endif
}

bool preflight_fmp4_bootstrap(const CaptureParams &p,
                              std::shared_ptr<Session> session,
                              std::string &error) {
#ifdef HAS_OPENH264
  if (!session->capture || !session->capture->running()) {
    error = "capture not running";
    return false;
  }
  if (!session->sps.empty() && !session->pps.empty()) {
    return true;
  }

  H264Encoder encoder;
  if (!encoder.init(p)) {
    error = "h264 encoder init failed";
    return false;
  }
  encoder.force_idr();

  const int y_size = p.width * p.height;
  const int uv_size = (p.width / 2) * (p.height / 2);
  std::string frame;
  std::string yuv;
  yuv.resize(y_size + 2 * uv_size);
  uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
  uint8_t *u = y + y_size;
  uint8_t *v = u + uv_size;

  constexpr int kTries = 200;
  for (int i = 0; i < kTries; ++i) {
    if (!session->capture->latest_frame(frame)) {
      std::this_thread::sleep_for(10ms);
      continue;
    }
    PixelFormat fmt = session->capture->pixel_format();
    if (fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) {
      error =
          std::string("unsupported pixel format: ") + pixel_format_label(fmt);
      return false;
    }
    if (fmt == PixelFormat::YUYV) {
      yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()), p.width,
                   p.height, y, u, v);
    } else {
      const uint8_t *src_y = reinterpret_cast<const uint8_t *>(frame.data());
      const uint8_t *src_uv = src_y + (p.width * p.height);
      nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width, y, u, v);
    }
    std::string nal_annexb;
    if (!encoder.encode_i420(y, u, v, nal_annexb)) {
      std::this_thread::sleep_for(10ms);
      continue;
    }
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    extract_sps_pps(nal_annexb, sps, pps);
    if (!sps.empty() && !pps.empty()) {
      session->sps = std::move(sps);
      session->pps = std::move(pps);
      return true;
    }
  }

  error = "timed out waiting for SPS/PPS";
  return false;
#else
  (void)p;
  (void)session;
  error = "OpenH264 not enabled";
  return false;
#endif
}

int main(int argc, char *argv[]) {
  struct Config {
    std::string addr = "0.0.0.0";
    int port = 8080;
    int idle_timeout = 10;
    std::string default_codec = "mjpeg";
  } cfg;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--addr" && i + 1 < argc) {
      cfg.addr = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      cfg.port = std::stoi(argv[++i]);
    } else if (arg == "--idle-timeout" && i + 1 < argc) {
      cfg.idle_timeout = std::stoi(argv[++i]);
    } else if (arg == "--codec" && i + 1 < argc) {
      cfg.default_codec = std::string(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "SilkCast\n"
                << "  --addr <ip>          Bind address (default 0.0.0.0)\n"
                << "  --port <port>        Bind port   (default 8080)\n"
                << "  --idle-timeout <s>   Idle seconds before closing device "
                   "(default 10)\n"
                << "  --codec <mjpeg|h264> Default codec if not specified "
                   "(default mjpeg)\n";
      return 0;
    }
  }

  SessionManager sessions(cfg.idle_timeout);
  httplib::Server svr;
  ApiRouter api;

  // Streaming endpoints require chunked transfer encoding hacks and range
  // clearing
  svr.set_pre_routing_handler(
      [](const httplib::Request &req, httplib::Response &res) {
        (void)res;
        if (req.path.rfind("/stream/live/", 0) == 0 ||
            req.path.rfind("/stream/ws", 0) == 0) {
          auto &mutable_req = const_cast<httplib::Request &>(req);
          mutable_req.ranges.clear();
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });

  // Serve the dynamic index page
  api.add_route({"/",
                 "GET",
                 "Interactive API Reference",
                 {},
                 [](const httplib::Request &, httplib::Response &res) {
                   res.set_header("Content-Type", "text/html; charset=utf-8");
                   res.status = 200;
                   res.set_content(kIndexHtml, "text/html");
                 }});

  api.add_route({"/device/list",
                 "GET",
                 "List available video devices",
                 {},
                 [&sessions](const httplib::Request &, httplib::Response &res) {
                   auto devices = sessions.list_devices();
                   res.set_header("Content-Type", "application/json");
                   res.status = 200;
                   res.set_content(json_array(devices), "application/json");
                 }});

  api.add_route(
      {"/device/{device}/caps",
       "GET",
       "Get device native capabilities",
       {{"device", ParamType::Device, "video0", "Device ID"}},
       [&sessions](const httplib::Request &req, httplib::Response &res) {
         if (req.matches.size() < 2) {
           res.status = 404;
           return;
         }
         std::string device_id = req.matches[1].str();
         sessions.touch(device_id);
#ifdef __linux__
         std::string error;
         auto json = build_device_caps_json(device_id, error);
         if (json.empty()) {
           res.status = 503;
           res.set_content(build_error_json("caps_unavailable", error),
                           "application/json");
           return;
         }
         res.status = 200;
         res.set_header("Content-Type", "application/json");
         res.set_content(json, "application/json");
#else
         (void)device_id;
         res.status = 503;
         res.set_content(
             build_error_json("caps_unavailable",
                              "device capabilities supported on Linux only"),
             "application/json");
#endif
       }});

  // Stats Route
  api.add_route(
      {"/stream/{device}/stats",
       "GET",
       "Get stream statistics",
       {{"device", ParamType::Device, "video0", "Device ID"}},
       [&sessions](const httplib::Request &req, httplib::Response &res) {
         if (req.matches.size() < 2) {
           res.status = 404;
           return;
         }
         std::string device_id = req.matches[1].str();
         auto session_opt = sessions.find(device_id);
         if (!session_opt) {
           res.status = 404;
           res.set_content(build_error_json("not_found",
                                            "device " + std::string(device_id)),
                           "application/json");
           return;
         }
         auto session = *session_opt;
         sessions.touch(device_id);
         const auto now = std::chrono::steady_clock::now();
         double uptime = std::chrono::duration_cast<std::chrono::seconds>(
                             now - session->started)
                             .count();
         if (uptime < 0.001)
           uptime = 0.001;
         double fps = session->frames_sent.load() / uptime;
         double bitrate_kbps =
             (session->bytes_sent.load() * 8.0 / 1000.0) / uptime;

         res.status = 200;
         res.set_content("{"
                         "\"device\":\"" +
                             session->device_id +
                             "\","
                             "\"codec\":\"" +
                             session->params.codec +
                             "\","
                             "\"pixel_format\":\"" +
                             std::string(pixel_format_label(
                                 session->pixel_format)) +
                             "\","
                             "\"width\":" +
                             std::to_string(session->params.width) +
                             ","
                             "\"height\":" +
                             std::to_string(session->params.height) +
                             ","
                             "\"fps\":" +
                             std::to_string(session->params.fps) +
                             ","
                             "\"bitrate_kbps\":" +
                             std::to_string(session->params.bitrate_kbps) +
                             ","
                             "\"active_clients\":" +
                             std::to_string(session->client_count.load()) +
                             ","
                             "\"fps_out\":" +
                             std::to_string(fps) +
                             ","
                             "\"bitrate_out_kbps\":" +
                             std::to_string(bitrate_kbps) +
                             ","
                             "\"frames_sent\":" +
                             std::to_string(session->frames_sent.load()) +
                             ","
                             "\"bytes_sent\":" +
                             std::to_string(session->bytes_sent.load()) + "}",
                         "application/json");
       }});

  // Live Stream Route
  api.add_route(
      {"/stream/live/{device}",
       "GET",
       "Start a live stream",
       {{"device", ParamType::Device, "video0", "Device ID"},
        {"w", ParamType::Int, "1280", "Width"},
        {"h", ParamType::Int, "720", "Height"},
        {"fps", ParamType::Int, "30", "Framerate"},
        {"bitrate", ParamType::Int, "256", "Bitrate (kbps)"},
        {"gop", ParamType::Int, "30", "GOP Size"},
        {"codec", ParamType::Select, "mjpeg", "Video Codec", {"mjpeg", "h264"}},
        {"latency",
         ParamType::Select,
         "view",
         "Latency Mode",
         {"view", "low", "ultra"}},
        {"container",
         ParamType::Select,
         "raw",
         "Container Format",
         {"raw", "mp4"}}},
       [&sessions](const httplib::Request &req, httplib::Response &res) {
         if (req.matches.size() < 2) {
           res.status = 404;
           return;
         }
         std::string device_id = req.matches[1].str();
         auto params = parse_params(req);
         if (params.codec.empty())
           params.codec = "mjpeg";
         if (params.container.empty())
           params.container = "raw";

         auto session = sessions.get_or_create(device_id, params);
         session->client_count.fetch_add(1);
         session->last_accessed = std::chrono::steady_clock::now();

         EffectiveParams eff{params, session->params};
         eff.actual.container = params.container;
         add_effective_headers(res, eff);

         if (params.codec != session->params.codec) {
           res.status = 409;
           res.set_content(
               build_error_json("conflict", "params locked by first requester"),
               "application/json");
           session->client_count.fetch_sub(1);
           return;
         }

         auto on_done = [device_id, &sessions](bool) {
           auto session_opt = sessions.find(device_id);
           if (session_opt) {
             (*session_opt)->client_count.fetch_sub(1);
             sessions.release_if_idle(device_id);
           }
         };

        if (!session->capture->running()) {
          if (!session->capture->start(device_id, session->params)) {
            res.status = 503;
            res.set_content(build_error_json("device_unavailable",
                                             "failed to open camera"),
                            "application/json");
            session->client_count.fetch_sub(1);
            return;
          }
          sync_session_params(*session);
          session->started = std::chrono::steady_clock::now();
          session->frames_sent = 0;
          session->bytes_sent = 0;
        }

         EffectiveParams eff_actual{params, session->params};
         eff_actual.actual.container = params.container;
         add_effective_headers(res, eff_actual);

         if (params.container == "mp4" && params.codec != "h264") {
           res.status = 400;
           res.set_content(
               build_error_json("bad_request", "mp4 container requires h264"),
               "application/json");
           session->client_count.fetch_sub(1);
           sessions.release_if_idle(device_id);
           return;
         }

         if (params.codec == "mjpeg") {
           serve_mjpeg_live(session->params, res, session, on_done);
         } else if (params.codec == "h264") {
           if (params.container == "mp4") {
             std::string error;
             if (!preflight_fmp4_bootstrap(session->params, session, error)) {
               res.status = 503;
               res.set_content(build_error_json("fmp4_unavailable", error),
                               "application/json");
               session->client_count.fetch_sub(1);
               sessions.release_if_idle(device_id);
               return;
             }
             serve_fmp4_live(session->params, res, session, on_done);
           } else {
             serve_h264_live(session->params, res, session, on_done);
           }
         } else {
           res.status = 400;
           res.set_content(build_error_json("bad_request", "unsupported codec"),
                           "application/json");
           session->client_count.fetch_sub(1);
           sessions.release_if_idle(device_id);
         }
       }});

  // UDP Stream Route
  api.add_route(
      {"/stream/udp/{device}",
       "GET",
       "Start a UDP stream (Linux only)",
       {{"device", ParamType::Device, "video0", "Device ID"},
        {"target", ParamType::String, "127.0.0.1", "Target IP"},
        {"port", ParamType::Int, "5000", "Target Port"},
        {"duration", ParamType::Int, "10", "Duration (seconds)"},
        {"w", ParamType::Int, "1280", "Width"},
        {"h", ParamType::Int, "720", "Height"},
        {"fps", ParamType::Int, "30", "Framerate"},
        {"bitrate", ParamType::Int, "2000", "Bitrate (kbps)"},
        {"gop", ParamType::Int, "30", "GOP Size"},
        {"codec", ParamType::Select, "h264", "Video Codec", {"h264", "mjpeg"}}},
       [&sessions](const httplib::Request &req, httplib::Response &res) {
#ifdef __linux__
         if (req.matches.size() < 2) {
           res.status = 404;
           return;
         }
         std::string device_id = req.matches[1].str();
         if (!req.has_param("target") || !req.has_param("port")) {
           res.status = 400;
           res.set_content(
               build_error_json("bad_request", "target and port are required"),
               "application/json");
           return;
         }
         const std::string target = req.get_param_value("target");
         int port = std::stoi(req.get_param_value("port"));
         int duration_sec = req.has_param("duration")
                                ? std::stoi(req.get_param_value("duration"))
                                : 10;
         auto params = parse_params(req);
         if (params.codec.empty())
           params.codec = "h264";

         auto session = sessions.get_or_create(device_id, params);
         session->client_count.fetch_add(1);
         session->last_accessed = std::chrono::steady_clock::now();

         if (!session->capture->running()) {
           if (!session->capture->start(device_id, session->params)) {
             res.status = 503;
             res.set_content(build_error_json("device_unavailable",
                                              "failed to open camera"),
                             "application/json");
             session->client_count.fetch_sub(1);
             return;
           }
           sync_session_params(*session);
           session->started = std::chrono::steady_clock::now();
           session->frames_sent = 0;
           session->bytes_sent = 0;
         }

         std::thread([session, params, target, port, duration_sec,
                      &sessions]() {
           int sock = socket(AF_INET, SOCK_DGRAM, 0);
           if (sock < 0) {
             session->client_count.fetch_sub(1);
             sessions.release_if_idle(session->device_id);
             return;
           }
           sockaddr_in addr{};
           addr.sin_family = AF_INET;
           addr.sin_port = htons(port);
           if (inet_pton(AF_INET, target.c_str(), &addr.sin_addr) != 1) {
             close(sock);
             session->client_count.fetch_sub(1);
             sessions.release_if_idle(session->device_id);
             return;
           }

           std::string frame;
           std::string yuv;
           const int y_size = params.width * params.height;
           const int uv_size = (params.width / 2) * (params.height / 2);
           yuv.resize(y_size + 2 * uv_size);
           uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
           uint8_t *u = y + y_size;
           uint8_t *v = u + uv_size;
           bool first = true;
#ifdef HAS_OPENH264
           H264Encoder encoder;
           bool encoder_ready = false;
           if (params.codec == "h264") {
             if (!encoder.init(session->params))
               encoder_ready = false;
             else {
               encoder.force_idr();
               encoder_ready = true;
               first = false;
             }
           }
#endif
           auto start = std::chrono::steady_clock::now();
           const int frame_interval_ms =
               std::max(1, 1000 / std::max(1, params.fps));
           const size_t mtu = 1400;

           while (true) {
             auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
             if (elapsed >= duration_sec)
               break;
             if (!session->capture || !session->capture->running()) {
               std::this_thread::sleep_for(10ms);
               continue;
             }
             if (!session->capture->latest_frame(frame)) {
               std::this_thread::sleep_for(5ms);
               continue;
             }

             if (params.codec == "mjpeg") {
               size_t offset = 0;
               while (offset < frame.size()) {
                 size_t chunk = std::min(mtu, frame.size() - offset);
                 sendto(sock, frame.data() + offset, chunk, 0,
                        reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
                 offset += chunk;
               }
               session->frames_sent.fetch_add(1);
               session->bytes_sent.fetch_add(frame.size());
             } else if (params.codec == "h264") {
#ifdef HAS_OPENH264
               if (!encoder_ready)
                 break;
               PixelFormat fmt = session->capture->pixel_format();
               if (fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) {
                 std::this_thread::sleep_for(5ms);
                 continue;
               }
               if (fmt == PixelFormat::YUYV) {
                 yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                              params.width, params.height, y, u, v);
               } else {
                 const uint8_t *src_y =
                     reinterpret_cast<const uint8_t *>(frame.data());
                 const uint8_t *src_uv = src_y + (params.width * params.height);
                 nv12_to_i420(src_y, src_uv, params.width, params.height,
                              params.width, params.width, y, u, v);
               }
               if (first) {
                 encoder.force_idr();
                 first = false;
               }
               std::string nal;
               if (!encoder.encode_i420(y, u, v, nal)) {
                 std::this_thread::sleep_for(5ms);
                 continue;
               }
               static const char start_code[] = {0x00, 0x00, 0x00, 0x01};
               std::string packet;
               packet.reserve(sizeof(start_code) + nal.size());
               packet.append(start_code, sizeof(start_code));
               packet.append(nal);
               sendto(sock, packet.data(), packet.size(), 0,
                      reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
               session->frames_sent.fetch_add(1);
               session->bytes_sent.fetch_add(packet.size());
#else
               break;
#endif
             } else {
               break;
             }
             session->last_accessed = std::chrono::steady_clock::now();
             std::this_thread::sleep_for(
                 std::chrono::milliseconds(frame_interval_ms));
           }
           close(sock);
           session->client_count.fetch_sub(1);
           sessions.release_if_idle(session->device_id);
         }).detach();

         res.status = 200;
         res.set_content("{\"status\":\"udp_stream_started\"}",
                         "application/json");
#else
         (void)req;
         res.status = 503;
         res.set_content(build_error_json("udp_unavailable",
                                          "UDP sender supported on Linux only"),
                         "application/json");
#endif
       }});

  api.register_with(svr);

  std::cout << "SilkCast server listening on " << cfg.addr << ":" << cfg.port
            << " (idle-timeout=" << cfg.idle_timeout << "s)" << std::endl;
  svr.listen(cfg.addr.c_str(), cfg.port);
  return 0;
}
