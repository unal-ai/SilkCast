#include "stream_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

#include "api_router.hpp"
#include "capture_v4l2.hpp"
#include "encoder_h264.hpp"
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
  default:
    return "unknown";
  }
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
  if (req.has_param("quality"))
    p.quality = std::stoi(req.get_param_value("quality"));
  if (req.has_param("gop"))
    p.gop = std::stoi(req.get_param_value("gop"));
  if (req.has_param("codec"))
    p.codec = req.get_param_value("codec");
  if (req.has_param("latency"))
    p.latency = req.get_param_value("latency");
  if (req.has_param("container"))
    p.container = req.get_param_value("container");
  apply_latency_preset(p);
  return p;
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
  res.set_header("Effective-Params",
                 "codec=" + a.codec + ";w=" + std::to_string(a.width) +
                     ";h=" + std::to_string(a.height) +
                     ";fps=" + std::to_string(a.fps) +
                     ";bitrate=" + std::to_string(a.bitrate_kbps) +
                     ";quality=" + std::to_string(a.quality) +
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
        std::unique_ptr<Mp4Fragmenter> mux_guard;
        Mp4Fragmenter *mux = nullptr;
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

} // namespace stream
