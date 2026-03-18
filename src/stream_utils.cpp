#include "stream_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "api_router.hpp"
#include "capture_v4l2.hpp"
#include "encoder_h264.hpp"
#include "encoder_jpeg.hpp"
#include "mp4_frag.hpp"
#include "types.hpp"
#include "yuv_convert.hpp"

using namespace std::chrono_literals;

namespace stream {
namespace {
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

std::string normalize_raw_pixfmt(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string raw_content_type(const std::string &pixfmt) {
  if (pixfmt == "rgb24") {
    return "video/raw; format=rgb24";
  }
  return "video/raw; format=i420";
}
} // namespace

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
                             const std::string &details) {
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
  case PixelFormat::H264:
    return "h264";
  default:
    return "unknown";
  }
}

std::string url_decode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      int v = 0;
      std::istringstream iss(in.substr(i + 1, 2));
      if (iss >> std::hex >> v) {
        out.push_back(static_cast<char>(v));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i] == '+' ? ' ' : in[i]);
  }
  return out;
}

std::string get_local_ip_address() {
#if defined(__linux__) || defined(__APPLE__)
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return "127.0.0.1";

  std::string ip = "127.0.0.1";
  for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) continue;
    if (ifa->ifa_addr->sa_family != AF_INET) continue;
    if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;

    char host[NI_MAXHOST] = {0};
    int rc = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                         NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
    if (rc == 0) {
      ip = host;
      break;
    }
  }

  freeifaddrs(ifaddr);
  return ip;
#else
  return "127.0.0.1";
#endif
}

#ifdef __linux__
namespace {
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
} // namespace

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
    for (fsize.index = 0; xioctl_device(fd, VIDIOC_ENUM_FRAMESIZES, &fsize);
         ++fsize.index) {
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
               << ",\"denominator\":" << ival.stepwise.step.denominator << "}}";
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
#endif // __linux__

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

bool contains_idr(const std::string &bitstream) {
  if (bitstream.empty()) return false;

  auto is_start_code = [&bitstream](size_t pos) {
    return pos + 2 < bitstream.size() && bitstream[pos] == 0 &&
           bitstream[pos + 1] == 0 &&
           (bitstream[pos + 2] == 1 ||
            (bitstream[pos + 2] == 0 && pos + 3 < bitstream.size() &&
             bitstream[pos + 3] == 1));
  };

  bool saw_start_code = false;
  for (size_t i = 0; i + 3 < bitstream.size(); ++i) {
    if (!is_start_code(i)) continue;
    saw_start_code = true;
    const size_t sc = (bitstream[i + 2] == 1) ? 3 : 4;
    const size_t nal_pos = i + sc;
    if (nal_pos >= bitstream.size()) break;
    const uint8_t nal_type = static_cast<uint8_t>(bitstream[nal_pos]) & 0x1F;
    if (nal_type == 5) return true;
  }

  if (!saw_start_code) {
    const uint8_t nal_type = static_cast<uint8_t>(bitstream[0]) & 0x1F;
    return nal_type == 5;
  }
  return false;
}

namespace {
std::string build_param_error_json(const std::string &field,
                                   const std::string &value,
                                   const std::string &expected,
                                   const std::string &details) {
  return "{"
         "\"error\":\"bad_request\","
         "\"details\":\"" +
         json_escape(details) +
         "\","
         "\"field\":\"" +
         json_escape(field) +
         "\","
         "\"value\":\"" +
         json_escape(value) +
         "\","
         "\"expected\":\"" +
         json_escape(expected) + "\""
         "}";
}

bool parse_int_param(const httplib::Request &req, const char *name,
                     int min_value, int max_value, int &out,
                     std::string &error_json) {
  if (!req.has_param(name)) {
    return true;
  }
  const std::string value = req.get_param_value(name);
  try {
    size_t pos = 0;
    const long long parsed = std::stoll(value, &pos, 10);
    if (pos != value.size()) {
      error_json = build_param_error_json(
          name, value, "integer",
          std::string("parameter '") + name + "' must be an integer");
      return false;
    }
    if (parsed < min_value || parsed > max_value) {
      error_json = build_param_error_json(
          name, value,
          std::string("integer in [") + std::to_string(min_value) + "," +
              std::to_string(max_value) + "]",
          std::string("parameter '") + name + "' is out of range");
      return false;
    }
    out = static_cast<int>(parsed);
    return true;
  } catch (...) {
    error_json = build_param_error_json(
        name, value, "integer",
        std::string("parameter '") + name + "' must be an integer");
    return false;
  }
}
} // namespace

bool parse_params(const httplib::Request &req, CaptureParams &out,
                  std::string &error_json) {
  CaptureParams p;
  if (!parse_int_param(req, "w", 1, 16384, p.width, error_json))
    return false;
  if (!parse_int_param(req, "h", 1, 16384, p.height, error_json))
    return false;
  if (!parse_int_param(req, "fps", 1, 240, p.fps, error_json))
    return false;
  if (!parse_int_param(req, "bitrate", 1, 1000000, p.bitrate_kbps, error_json))
    return false;
  if (!parse_int_param(req, "quality", 1, 100, p.quality, error_json))
    return false;
  if (!parse_int_param(req, "gop", 1, 1000, p.gop, error_json))
    return false;
  if (req.has_param("media"))
    p.media = req.get_param_value("media");
  if (req.has_param("codec"))
    p.codec = req.get_param_value("codec");
  if (req.has_param("source_codec"))
    p.source_codec = req.get_param_value("source_codec");
  if (req.has_param("latency"))
    p.latency = req.get_param_value("latency");
  if (req.has_param("container"))
    p.container = req.get_param_value("container");
  if (req.has_param("pixfmt"))
    p.pixfmt = req.get_param_value("pixfmt");
  auto to_lower_copy = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  };
  p.media = to_lower_copy(p.media);
  p.codec = to_lower_copy(p.codec);
  p.source_codec = to_lower_copy(p.source_codec);
  p.latency = to_lower_copy(p.latency);
  p.container = to_lower_copy(p.container);
  p.pixfmt = normalize_raw_pixfmt(p.pixfmt);
  if (p.source_codec == "auto")
    p.source_codec.clear();
  if (!p.source_codec.empty() && p.source_codec != "mjpeg" &&
      p.source_codec != "raw") {
    error_json = build_param_error_json(
        "source_codec", p.source_codec, "auto|mjpeg|raw",
        "parameter 'source_codec' must be one of: auto, mjpeg, raw");
    return false;
  }
  apply_latency_preset(p);
  if (p.codec == "raw") {
    if (p.pixfmt.empty()) {
      p.pixfmt = "i420";
    } else if (!is_supported_raw_pixfmt(p.pixfmt)) {
      error_json = build_param_error_json(
          "pixfmt", p.pixfmt, "i420|rgb24",
          "parameter 'pixfmt' must be one of: i420, rgb24");
      return false;
    }
  }
  out = std::move(p);
  return true;
}

void apply_latency_preset(CaptureParams &p) {
  if (p.latency == "zerolatency") {
    if (p.codec.empty() || p.codec == "mjpeg")
      p.codec = "h264";
    if (p.container == "mp4")
      p.container = "raw";
    p.gop = 1;
    if (p.bitrate_kbps < 512)
      p.bitrate_kbps = 512;
    p.latency = "ultra";
  }
}

bool is_supported_raw_pixfmt(const std::string &pixfmt) {
  return pixfmt == "i420" || pixfmt == "rgb24";
}

size_t raw_frame_size_bytes(const CaptureParams &p) {
  const size_t width = static_cast<size_t>(std::max(0, p.width));
  const size_t height = static_cast<size_t>(std::max(0, p.height));
  if (p.pixfmt == "rgb24") {
    return width * height * 3;
  }
  return width * height + ((width / 2) * (height / 2) * 2);
}

CaptureParams normalize_source_params(const std::string &device_id,
                                      const CaptureParams &requested) {
  CaptureParams source = requested;
  const bool is_rtsp = device_id.rfind("rtsp://", 0) == 0;
  if (is_rtsp) {
    source.codec = "h264";
  } else if (!requested.source_codec.empty()) {
    source.codec = requested.source_codec;
  } else if (requested.codec == "mjpeg") {
    // When JPEG re-encoding is available, prefer a raw-backed source for the
    // default "auto" MJPEG path. Explicit source_codec=mjpeg still preserves
    // true passthrough for clients that want camera-native MJPEG.
#ifdef HAS_JPEG
    source.codec = "raw";
#else
    source.codec = "mjpeg";
#endif
  } else {
    source.codec = "raw";
  }
  source.source_codec.clear();
  source.pixfmt.clear();
  source.container = "raw";
  if (source.codec != "mjpeg") {
    source.quality = 80;
  }
  source.bitrate_kbps = 256;
  source.gop = 30;
  source.latency = "view";
  return source;
}

bool session_can_serve_request(const std::string &device_id,
                               const Session &session,
                               const CaptureParams &requested) {
  const CaptureParams requested_source =
      normalize_source_params(device_id, requested);
  const CaptureParams &active_source = session.params;
  const bool auto_mjpeg_can_reuse_raw_source =
      requested.source_codec.empty() && requested.codec == "mjpeg" &&
      active_source.codec == "raw";
  const bool fps_compatible =
      requested_source.fps == active_source.fps ||
      (active_source.codec == "raw" && requested_source.fps > 0 &&
       requested_source.fps <= active_source.fps);
  const bool same_source =
      requested_source.width == active_source.width &&
      requested_source.height == active_source.height &&
      fps_compatible &&
      requested_source.media == active_source.media &&
      (requested_source.codec == active_source.codec ||
       auto_mjpeg_can_reuse_raw_source) &&
      (requested_source.codec != "mjpeg" ||
       auto_mjpeg_can_reuse_raw_source ||
       requested_source.quality == active_source.quality);
  if (!same_source) {
    return false;
  }
  if (device_id.rfind("rtsp://", 0) == 0) {
    return requested.codec == "h264";
  }
  if (active_source.codec == "raw") {
    return requested.codec == "raw" || requested.codec == "h264" ||
           requested.codec == "mjpeg";
  }
  if (active_source.codec == "mjpeg") {
    return requested.codec == "mjpeg";
  }
  if (active_source.codec == "h264") {
    return requested.codec == "h264";
  }
  return false;
}

CaptureParams derive_effective_output_params(const Session &session,
                                             const CaptureParams &requested) {
  CaptureParams effective = requested;
  effective.width = session.params.width;
  effective.height = session.params.height;
  if (requested.fps > 0 && session.params.fps > 0) {
    effective.fps = std::min(requested.fps, session.params.fps);
  } else {
    effective.fps = session.params.fps;
  }
  if (effective.codec == "h264" && requested.container == "mp4") {
    effective.container = "mp4";
  } else {
    effective.container = "raw";
  }
  if (effective.codec != "raw") {
    effective.pixfmt.clear();
  } else if (effective.pixfmt.empty()) {
    effective.pixfmt = "i420";
  }
  return effective;
}

void sync_session_params(Session &session) {
  if (!session.capture)
    return;
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
  res.set_header(
      "Effective-Params",
      "media=" + a.media + ";codec=" + a.codec + ";w=" + std::to_string(a.width) +
          ";h=" + std::to_string(a.height) + ";fps=" + std::to_string(a.fps) +
          ";bitrate=" + std::to_string(a.bitrate_kbps) +
          ";quality=" + std::to_string(a.quality) +
          ";gop=" + std::to_string(a.gop) + ";latency=" + a.latency +
          ";container=" + a.container +
          (a.pixfmt.empty() ? "" : ";pixfmt=" + a.pixfmt));
}

void note_frame_sent(Session &session, size_t bytes, bool is_iframe) {
  session.frames_sent.fetch_add(1);
  if (bytes > 0) {
    session.bytes_sent.fetch_add(bytes);
  }
  session.last_accessed = std::chrono::steady_clock::now();
  if (!session.first_frame_marked.exchange(true)) {
    const auto delta_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - session.started)
            .count();
    if (delta_ms >= 0) {
      session.first_frame_ms.store(static_cast<uint64_t>(delta_ms));
    }
  }

  if (session.params.codec == "h264" && is_iframe &&
      !session.first_iframe_marked.exchange(true)) {
    const auto delta_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - session.started)
            .count();
    if (delta_ms >= 0) {
      session.first_iframe_ms.store(static_cast<uint64_t>(delta_ms));
    }
  }
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
          note_frame_sent(*session, prefix.size() + sizeof(kTinyJpeg) + 2);
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
        JpegFrameEncoder jpeg_encoder;
        std::string prefix;
        std::string frame;
        std::string jpeg_frame;
        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          const PixelFormat fmt = session->capture->pixel_format();
          if ((fmt != PixelFormat::MJPEG && fmt != PixelFormat::YUYV &&
               fmt != PixelFormat::NV12) ||
              !session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          if (!jpeg_encoder.encode(fmt, frame, p.width, p.height, p.quality,
                                   jpeg_frame)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          prefix = "--" + std::string(boundary) +
                   "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                   std::to_string(jpeg_frame.size()) + "\r\n\r\n";
          if (!sink.write(prefix.data(), prefix.size()))
            return false;
          if (!sink.write(jpeg_frame.data(), jpeg_frame.size()))
            return false;
          if (!sink.write("\r\n", 2))
            return false;
          note_frame_sent(*session, prefix.size() + jpeg_frame.size() + 2);
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
}

void serve_raw_live(const CaptureParams &p, httplib::Response &res,
                    std::shared_ptr<Session> session,
                    std::function<void(bool)> on_done) {
  const auto boundary = "frame";
  const size_t payload_size = raw_frame_size_bytes(p);
  res.set_header("Connection", "close");
  res.set_header("X-SilkCast-Pixel-Format", p.pixfmt);
  res.set_header("X-SilkCast-Width", std::to_string(p.width));
  res.set_header("X-SilkCast-Height", std::to_string(p.height));
  res.set_header("X-SilkCast-Fps", std::to_string(p.fps));
  res.set_header("X-SilkCast-Frame-Bytes", std::to_string(payload_size));
  res.set_chunked_content_provider(
      "multipart/x-mixed-replace; boundary=" + std::string(boundary),
      [p, boundary, payload_size, session](size_t, httplib::DataSink &sink) mutable {
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        std::string frame;
        std::string i420(y_size + (2 * uv_size), '\0');
        std::string payload(payload_size, '\0');
        uint8_t *y = reinterpret_cast<uint8_t *>(i420.data());
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;
        const std::string prefix =
            "--" + std::string(boundary) +
            "\r\nContent-Type: " + raw_content_type(p.pixfmt) +
            "\r\nContent-Length: " +
            std::to_string(payload_size) + "\r\n\r\n";

        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          const PixelFormat fmt = session->capture->pixel_format();
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
            nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width,
                         y, u, v);
          }

          if (p.pixfmt == "rgb24") {
            i420_to_rgb24(y, u, v, p.width, p.height,
                          reinterpret_cast<uint8_t *>(payload.data()));
          } else {
            std::memcpy(payload.data(), i420.data(), payload.size());
          }

          if (!sink.write(prefix.data(), prefix.size())) return false;
          if (!sink.write(payload.data(), payload.size())) return false;
          if (!sink.write("\r\n", 2)) return false;
          note_frame_sent(*session, prefix.size() + payload.size() + 2);
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
        bool passthrough =
            session->capture &&
            session->capture->pixel_format() == PixelFormat::H264;
        bool encoder_ready = passthrough;
        if (!passthrough) {
          if (!encoder.init(p)) {
            return false;
          }
          encoder.force_idr();
          encoder_ready = true;
        }
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        if (!passthrough) yuv.resize(y_size + 2 * uv_size);
        uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;

        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        bool first = true;
        uint32_t last_idr = session->idr_request_seq.load();
        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          PixelFormat fmt = session->capture->pixel_format();
          passthrough = (fmt == PixelFormat::H264);
          if ((!passthrough && fmt != PixelFormat::YUYV &&
               fmt != PixelFormat::NV12) ||
              !session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          std::string nal;
          if (passthrough) {
            nal = frame;
          } else {
            if (fmt == PixelFormat::YUYV) {
              yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                           p.width, p.height, y, u, v);
            } else {
              const uint8_t *src_y =
                  reinterpret_cast<const uint8_t *>(frame.data());
              const uint8_t *src_uv = src_y + (p.width * p.height);
              nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width,
                           y, u, v);
            }
            if (first) {
              encoder.force_idr();
              first = false;
            }
            uint32_t current_idr = session->idr_request_seq.load();
            if (current_idr > last_idr) {
              encoder.force_idr();
              last_idr = current_idr;
            }
            if (!encoder.encode_i420(y, u, v, nal)) {
              std::this_thread::sleep_for(5ms);
              continue;
            }
          }
          if (!nal.empty()) {
            const bool keyframe = contains_idr(nal);
            if (passthrough) {
              if (!sink.write(nal.data(), nal.size())) return false;
              note_frame_sent(*session, nal.size(), keyframe);
            } else {
              static const char start_code[] = {0, 0, 0, 1};
              if (!sink.write(start_code, 4)) return false;
              if (!sink.write(nal.data(), nal.size())) return false;
              note_frame_sent(*session, 4 + nal.size(), keyframe);
            }
          }
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
#else
  (void)p;
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
        bool passthrough =
            session->capture &&
            session->capture->pixel_format() == PixelFormat::H264;
        if (!passthrough) {
          if (!encoder.init(p)) return false;
          encoder.force_idr();
        }

        uint32_t seqno = 1;
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        if (!passthrough) yuv.resize(y_size + 2 * uv_size);
        uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;
        bool sent_init = false;
        std::unique_ptr<Mp4Fragmenter> mux_guard;
        Mp4Fragmenter *mux = nullptr;
        uint64_t decode_time = 0;
        uint32_t last_idr = session->idr_request_seq.load();

        while (true) {
          if (!passthrough && session->idr_request_seq.load() > last_idr) {
            encoder.force_idr();
            last_idr = session->idr_request_seq.load();
          }
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          if (!session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          PixelFormat fmt = session->capture->pixel_format();
          passthrough = (fmt == PixelFormat::H264);
          if (!passthrough && fmt != PixelFormat::YUYV &&
              fmt != PixelFormat::NV12) {
            std::this_thread::sleep_for(5ms);
            continue;
          }

          std::string nal_annexb;
          if (passthrough) {
            nal_annexb = frame;
            if (session->sps.empty() || session->pps.empty()) {
              std::vector<uint8_t> sps;
              std::vector<uint8_t> pps;
              session->capture->get_sps_pps(sps, pps);
              if (!sps.empty() && !pps.empty()) {
                session->sps = std::move(sps);
                session->pps = std::move(pps);
              }
            }
          } else {
            if (fmt == PixelFormat::YUYV) {
              yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                           p.width, p.height, y, u, v);
            } else {
              const uint8_t *src_y =
                  reinterpret_cast<const uint8_t *>(frame.data());
              const uint8_t *src_uv = src_y + (p.width * p.height);
              nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width,
                           y, u, v);
            }
            if (!encoder.encode_i420(y, u, v, nal_annexb)) {
              std::this_thread::sleep_for(5ms);
              continue;
            }
          }

          if (session->sps.empty() || session->pps.empty()) {
            extract_sps_pps(nal_annexb, session->sps, session->pps);
          }
          if (!mux && !session->sps.empty() && !session->pps.empty()) {
            mux_guard = std::make_unique<Mp4Fragmenter>(
                p.width, p.height, p.fps, session->sps, session->pps);
            mux = mux_guard.get();
          }
          if (!mux) {
            continue;
          }
          if (!sent_init) {
            auto init_seg = mux->build_init_segment();
            if (!sink.write(init_seg.data(), init_seg.size())) return false;
            sent_init = true;
          }
          auto avcc = annexb_to_avcc(nal_annexb);

          const bool keyframe = contains_idr(nal_annexb);

          auto frag = mux->build_fragment(avcc, seqno++, decode_time,
                                          sample_duration, keyframe);
          decode_time += sample_duration;
          if (!sink.write(frag.data(), frag.size()))
            return false;

          note_frame_sent(*session, frag.size(), keyframe);
          std::this_thread::sleep_for(
              std::chrono::milliseconds(1000 / std::max(1, p.fps)));
        }
        return true;
      },
      on_done);
#else
  (void)p;
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

  // RTSP/H264 pass-through path: SPS/PPS may come from SDP or in-band.
  if (session->capture->pixel_format() == PixelFormat::H264) {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    session->capture->get_sps_pps(sps, pps);
    if (!sps.empty() && !pps.empty()) {
      session->sps = std::move(sps);
      session->pps = std::move(pps);
      return true;
    }
    for (int i = 0; i < 80; ++i) {
      std::string frame;
      if (session->capture->latest_frame(frame)) {
        std::vector<uint8_t> fsps;
        std::vector<uint8_t> fpps;
        extract_sps_pps(frame, fsps, fpps);
        if (!fsps.empty() && !fpps.empty()) {
          session->sps = std::move(fsps);
          session->pps = std::move(fpps);
          return true;
        }
      }
      std::this_thread::sleep_for(25ms);
    }
    error = "timed out waiting for SPS/PPS from pass-through stream";
    return false;
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

} // namespace stream
