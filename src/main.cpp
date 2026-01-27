#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <algorithm>

#include "httplib.h"
#include "capture_v4l2.hpp"
#include "encoder_h264.hpp"
#include "types.hpp"
#include "yuv_convert.hpp"
#include "mp4_frag.hpp"
#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
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
    if (reaper_thread_.joinable()) reaper_thread_.join();
  }

  std::shared_ptr<Session> get_or_create(const std::string &device_id, const CaptureParams &params) {
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
        if (it->second->capture) it->second->capture->stop();
        sessions_.erase(it);
      }
    }
  }

  std::vector<std::string> list_devices() const {
    std::vector<std::string> devices;
    for (const auto &entry : std::filesystem::directory_iterator("/dev")) {
      const auto name = entry.path().filename().string();
      if (name.rfind("video", 0) == 0) {
        devices.push_back(name);
      }
    }
    if (devices.empty()) {
      devices.push_back("video0"); // fallback hint
    }
    return devices;
  }

  std::optional<std::shared_ptr<Session>> find(const std::string &device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(device_id);
    if (it == sessions_.end()) return std::nullopt;
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
          auto idle_for = std::chrono::duration_cast<std::chrono::seconds>(now - sess->last_accessed).count();
          if (sess->client_count.load() == 0 && idle_for > idle_timeout_seconds_) {
            if (sess->capture) sess->capture->stop();
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
    0xFF,0xD8,0xFF,0xDB,0x00,0x43,0x00,0x03,0x02,0x02,0x03,0x02,0x02,0x03,0x03,0x03,
    0x03,0x04,0x03,0x03,0x04,0x05,0x08,0x05,0x05,0x04,0x04,0x05,0x0A,0x07,0x07,0x06,
    0x08,0x0C,0x0A,0x0C,0x0C,0x0B,0x0A,0x0B,0x0B,0x0D,0x0E,0x12,0x10,0x0D,0x0E,0x11,
    0x0E,0x0B,0x0B,0x10,0x16,0x10,0x11,0x13,0x14,0x15,0x15,0x15,0x0C,0x0F,0x17,0x18,
    0x16,0x14,0x18,0x12,0x14,0x15,0x14,0xFF,0xC0,0x00,0x11,0x08,0x00,0x01,0x00,0x01,
    0x03,0x01,0x11,0x00,0x02,0x11,0x01,0x03,0x11,0x01,0xFF,0xC4,0x00,0x14,0x00,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xFF,0xC4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xDA,0x00,0x0C,0x03,0x01,0x00,0x02,0x11,0x03,
    0x11,0x00,0x3F,0x00,0xFF,0xD9};

std::string json_array(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += "\"" + items[i] + "\"";
    if (i + 1 < items.size()) out += ",";
  }
  out += "]";
  return out;
}

std::string build_error_json(const std::string &msg, const std::string &details = "") {
  std::string out = "{\"error\":\"" + msg + "\"";
  if (!details.empty()) out += ",\"details\":\"" + details + "\"";
  out += "}";
  return out;
}

// Convert Annex-B NAL stream to length-prefixed (AVCC) single-sample buffer.
std::vector<uint8_t> annexb_to_avcc(const std::string& annexb) {
  std::vector<uint8_t> out;
  size_t i = 0;
  auto len = annexb.size();
  while (i + 3 < len) {
    // find start code
    if (!(annexb[i] == 0 && annexb[i+1] == 0 && ((annexb[i+2] == 1) || (annexb[i+2] == 0 && i+3 < len && annexb[i+3]==1)))) {
      ++i;
      continue;
    }
    size_t sc_size = (annexb[i+2] == 1) ? 3 : 4;
    i += sc_size;
    size_t start = i;
    while (i + 3 < len && !(annexb[i] == 0 && annexb[i+1] == 0 && ((annexb[i+2]==1) || (annexb[i+2]==0 && i+3 < len && annexb[i+3]==1)))) {
      ++i;
    }
    size_t nalsize = i - start;
    uint32_t n = static_cast<uint32_t>(nalsize);
    out.push_back((n >> 24) & 0xFF);
    out.push_back((n >> 16) & 0xFF);
    out.push_back((n >> 8) & 0xFF);
    out.push_back(n & 0xFF);
    out.insert(out.end(), annexb.begin() + start, annexb.begin() + start + nalsize);
  }
  return out;
}

// Extract SPS/PPS from Annex-B sample (first IDR)
void extract_sps_pps(const std::string& annexb, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
  size_t i = 0;
  auto len = annexb.size();
  while (i + 4 < len) {
    if (!(annexb[i]==0 && annexb[i+1]==0 && ((annexb[i+2]==1)|| (annexb[i+2]==0 && annexb[i+3]==1)))) { ++i; continue; }
    size_t sc_size = (annexb[i+2]==1)?3:4;
    i += sc_size;
    size_t start = i;
    while (i + 4 < len && !(annexb[i]==0 && annexb[i+1]==0 && ((annexb[i+2]==1)|| (annexb[i+2]==0 && annexb[i+3]==1)))) { ++i; }
    size_t nalsize = i - start;
    if (nalsize == 0) continue;
    uint8_t nal_type = annexb[start] & 0x1F;
    if (nal_type == 7 && sps.empty()) {
      sps.assign(annexb.begin() + start, annexb.begin() + start + nalsize);
    } else if (nal_type == 8 && pps.empty()) {
      pps.assign(annexb.begin() + start, annexb.begin() + start + nalsize);
    }
    if (!sps.empty() && !pps.empty()) break;
  }
}

CaptureParams parse_params(const httplib::Request &req) {
  CaptureParams p;
  if (req.has_param("w")) p.width = std::stoi(req.get_param_value("w"));
  if (req.has_param("h")) p.height = std::stoi(req.get_param_value("h"));
  if (req.has_param("fps")) p.fps = std::stoi(req.get_param_value("fps"));
  if (req.has_param("bitrate")) p.bitrate_kbps = std::stoi(req.get_param_value("bitrate"));
  if (req.has_param("gop")) p.gop = std::stoi(req.get_param_value("gop"));
  if (req.has_param("codec")) p.codec = req.get_param_value("codec");
  if (req.has_param("latency")) p.latency = req.get_param_value("latency");
  if (req.has_param("container")) p.container = req.get_param_value("container");
  return p;
}

void add_effective_headers(httplib::Response &res, const EffectiveParams &eff) {
  const auto &a = eff.actual;
  res.set_header("Effective-Params",
                 "codec=" + a.codec +
                 ";w=" + std::to_string(a.width) +
                 ";h=" + std::to_string(a.height) +
                 ";fps=" + std::to_string(a.fps) +
                 ";bitrate=" + std::to_string(a.bitrate_kbps) +
                 ";gop=" + std::to_string(a.gop) +
                 ";latency=" + a.latency);
}

void serve_mjpeg_placeholder(const CaptureParams &p, httplib::Response &res, std::shared_ptr<Session> session) {
  const auto boundary = "frame";
  res.set_header("Connection", "close");
  res.set_chunked_content_provider(
      "multipart/x-mixed-replace; boundary=" + std::string(boundary),
      [p, boundary, session](size_t, httplib::DataSink &sink) mutable {
        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        std::string prefix = "--" + std::string(boundary) + "\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(sizeof(kTinyJpeg)) + "\r\n\r\n";
        for (;;) {
          if (!sink.write(prefix.data(), prefix.size())) return false;
          if (!sink.write(reinterpret_cast<const char *>(kTinyJpeg), sizeof(kTinyJpeg))) return false;
          if (!sink.write("\r\n", 2)) return false;
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      [](bool) {});
}

void serve_mjpeg_live(const CaptureParams &p, httplib::Response &res, std::shared_ptr<Session> session) {
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
                   "\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(frame.size()) + "\r\n\r\n";
          if (!sink.write(prefix.data(), prefix.size())) return false;
          if (!sink.write(frame.data(), frame.size())) return false;
          if (!sink.write("\r\n", 2)) return false;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(prefix.size() + frame.size() + 2);
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      [](bool) {});
}

void serve_h264_live(const CaptureParams &p, httplib::Response &res, std::shared_ptr<Session> session) {
#ifdef HAS_OPENH264
  res.set_header("Connection", "close");
  res.set_header("Content-Type", "video/H264");
  res.set_chunked_content_provider(
      "video/H264",
      [p, session](size_t, httplib::DataSink &sink) mutable {
        if (!session->encoder) {
          session->encoder = std::make_shared<H264Encoder>();
          if (!session->encoder->init(session->params)) {
            return false;
          }
          session->encoder->force_idr();
        }
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        yuv.resize(y_size + 2 * uv_size);
        uint8_t* y = reinterpret_cast<uint8_t*>(yuv.data());
        uint8_t* u = y + y_size;
        uint8_t* v = u + uv_size;

        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        bool first = true;
        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          if (session->capture->pixel_format() != PixelFormat::YUYV ||
              !session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          yuyv_to_i420(reinterpret_cast<const uint8_t*>(frame.data()), p.width, p.height, y, u, v);
          if (first) {
            session->encoder->force_idr();
            first = false;
          }
          std::string nal;
          if (!session->encoder->encode_i420(reinterpret_cast<const uint8_t*>(y), u, v, nal)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          // Write Annex-B start code before NAL data.
          static const char start_code[] = {0x00, 0x00, 0x00, 0x01};
          if (!sink.write(start_code, sizeof(start_code))) return false;
          if (!sink.write(nal.data(), nal.size())) return false;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(sizeof(start_code) + nal.size());
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      [](bool) {});
#else
  (void)p;
  res.status = 503;
  res.set_content(build_error_json("h264_unavailable", "OpenH264 not enabled"), "application/json");
#endif
}

void serve_fmp4_live(const CaptureParams &p, httplib::Response &res, std::shared_ptr<Session> session) {
#ifdef HAS_OPENH264
  res.set_header("Connection", "close");
  res.set_header("Content-Type", "video/mp4");
  res.set_header("Cache-Control", "no-store");
  res.set_header("Access-Control-Allow-Origin", "*");
  const uint32_t sample_duration = p.fps > 0 ? (90000 / p.fps) : 6000;

  res.set_chunked_content_provider(
      "video/mp4",
      [p, session, sample_duration](size_t, httplib::DataSink &sink) mutable {
        if (!session->encoder) {
          session->encoder = std::make_shared<H264Encoder>();
          if (!session->encoder->init(session->params)) return false;
          session->encoder->force_idr();
        }
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        yuv.resize(y_size + 2 * uv_size);
        uint8_t* y = reinterpret_cast<uint8_t*>(yuv.data());
        uint8_t* u = y + y_size;
        uint8_t* v = u + uv_size;
        bool sent_init = false;
        Mp4Fragmenter* mux = nullptr;
        std::unique_ptr<Mp4Fragmenter> mux_guard;
        uint64_t decode_time = 0;
        while (true) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          if (!session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (session->capture->pixel_format() != PixelFormat::YUYV) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          yuyv_to_i420(reinterpret_cast<const uint8_t*>(frame.data()), p.width, p.height, y, u, v);
          std::string nal_annexb;
          if (!session->encoder->encode_i420(y, u, v, nal_annexb)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (session->sps.empty() || session->pps.empty()) {
            extract_sps_pps(nal_annexb, session->sps, session->pps);
            if (!session->sps.empty() && !session->pps.empty()) {
              mux_guard = std::make_unique<Mp4Fragmenter>(p.width, p.height, p.fps, session->sps, session->pps);
              mux = mux_guard.get();
            }
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
          bool keyframe = !nal_annexb.empty() && ((nal_annexb[4] & 0x1F) == 5);
          auto frag = mux->build_fragment(avcc, session->seqno++, decode_time, sample_duration, keyframe);
          decode_time += sample_duration;
          if (!sink.write(frag.data(), frag.size())) return false;

          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(frag.size());
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(std::chrono::milliseconds(1000 / std::max(1, p.fps)));
        }
        return true;
      },
      [](bool) {});
#else
  (void)p;
  res.status = 503;
  res.set_content(build_error_json("h264_unavailable", "OpenH264 not enabled"), "application/json");
#endif
}

int main(int argc, char* argv[]) {
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
      cfg.default_codec = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "SilkCast\n"
                << "  --addr <ip>          Bind address (default 0.0.0.0)\n"
                << "  --port <port>        Bind port   (default 8080)\n"
                << "  --idle-timeout <s>   Idle seconds before closing device (default 10)\n"
                << "  --codec <mjpeg|h264> Default codec if not specified (default mjpeg)\n";
      return 0;
    }
  }

  SessionManager sessions(cfg.idle_timeout);
  httplib::Server svr;

  // WebSocket low-latency stream (binary MJPEG or H.264 NALs)
  svr.set_ws_endpoint("/stream/ws", [&sessions](const httplib::Request &req, const std::shared_ptr<httplib::WebSocket> ws) {
    if (!req.has_param("id")) {
      ws->send("missing id", httplib::OpCode::CLOSE);
      return;
    }
    const auto device_id = req.get_param_value("id");
    auto params = parse_params(req);
    if (params.codec.empty()) params.codec = "mjpeg";
    auto session = sessions.get_or_create(device_id, params);
    session->client_count.fetch_add(1);
    session->last_accessed = std::chrono::steady_clock::now();

    if (params.codec != session->params.codec) {
      res.status = 409;
      res.set_content(build_error_json("conflict", "codec locked by first requester"), "application/json");
      session->client_count.fetch_sub(1);
      return;
    }

    if (params.codec != session->params.codec) {
      ws->send("conflict: codec locked", httplib::OpCode::CLOSE);
      session->client_count.fetch_sub(1);
      return;
    }

    // Start capture if needed.
    if (!session->capture->running()) {
      if (!session->capture->start(device_id, session->params)) {
        ws->send("camera_unavailable", httplib::OpCode::CLOSE);
        session->client_count.fetch_sub(1);
        return;
      }
      session->pixel_format = session->capture->pixel_format();
      session->started = std::chrono::steady_clock::now();
      session->frames_sent = 0;
      session->bytes_sent = 0;
    }

    std::thread([ws, session, params]() {
      const int frame_interval_ms = std::max(1, 1000 / std::max(1, params.fps));
      std::string frame;
      std::string yuv;
      const int y_size = params.width * params.height;
      const int uv_size = (params.width / 2) * (params.height / 2);
      yuv.resize(y_size + 2 * uv_size);
      uint8_t* y = reinterpret_cast<uint8_t*>(yuv.data());
      uint8_t* u = y + y_size;
      uint8_t* v = u + uv_size;
      bool first = true;

      while (true) {
        if (!session->capture || !session->capture->running()) {
          std::this_thread::sleep_for(10ms);
          continue;
        }
        if (!session->capture->latest_frame(frame)) {
          std::this_thread::sleep_for(5ms);
          continue;
        }
        if (params.codec == "mjpeg") {
          if (!ws->send(frame, httplib::OpCode::BINARY)) break;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(frame.size());
        } else if (params.codec == "h264") {
#ifdef HAS_OPENH264
          if (!session->encoder) {
            session->encoder = std::make_shared<H264Encoder>();
            if (!session->encoder->init(session->params)) {
              ws->send("encode_init_failed", httplib::OpCode::CLOSE);
              break;
            }
            session->encoder->force_idr();
            first = false;
          }
          if (session->capture->pixel_format() != PixelFormat::YUYV) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          yuyv_to_i420(reinterpret_cast<const uint8_t*>(frame.data()), params.width, params.height, y, u, v);
          if (first) { session->encoder->force_idr(); first = false; }
          std::string nal;
          if (!session->encoder->encode_i420(y, u, v, nal)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          static const char start_code[] = {0x00, 0x00, 0x00, 0x01};
          std::string packet;
          packet.reserve(sizeof(start_code) + nal.size());
          packet.append(start_code, sizeof(start_code));
          packet.append(nal);
          if (!ws->send(packet, httplib::OpCode::BINARY)) break;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(packet.size());
#else
          ws->send("h264_disabled", httplib::OpCode::CLOSE);
          break;
#endif
        } else {
          ws->send("unsupported_codec", httplib::OpCode::CLOSE);
          break;
        }
        session->last_accessed = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
      }
      session->client_count.fetch_sub(1);
      sessions.release_if_idle(session->device_id);
    }).detach();
  });
  svr.Get(R"(/device/list)", [&sessions](const httplib::Request &, httplib::Response &res) {
    auto devices = sessions.list_devices();
    res.set_header("Content-Type", "application/json");
    res.status = 200;
    res.set_content(json_array(devices), "application/json");
  });

  svr.Get(R"(/stream/([^/]+)/stats)", [&sessions](const httplib::Request &req, httplib::Response &res) {
    auto device_id = req.matches[1];
    auto session_opt = sessions.find(device_id);
    if (!session_opt) {
      res.status = 404;
      res.set_content(build_error_json("not_found", "device " + device_id), "application/json");
      return;
    }
    auto session = *session_opt;
    sessions.touch(device_id);
    const auto now = std::chrono::steady_clock::now();
    double uptime = std::chrono::duration_cast<std::chrono::seconds>(now - session->started).count();
    if (uptime < 0.001) uptime = 0.001;
    double fps = session->frames_sent.load() / uptime;
    double bitrate_kbps = (session->bytes_sent.load() * 8.0 / 1000.0) / uptime;

    res.status = 200;
    res.set_content(
        "{"
        "\"device\":\"" + session->device_id + "\","
        "\"fps\":" + std::to_string(session->params.fps) + ","
        "\"bitrate_kbps\":" + std::to_string(session->params.bitrate_kbps) + ","
        "\"active_clients\":" + std::to_string(session->client_count.load()) + ","
        "\"fps_out\":" + std::to_string(fps) + ","
        "\"bitrate_out_kbps\":" + std::to_string(bitrate_kbps) + ","
        "\"frames_sent\":" + std::to_string(session->frames_sent.load()) + ","
        "\"bytes_sent\":" + std::to_string(session->bytes_sent.load()) +
        "}",
        "application/json");
  });

  svr.Get(R"(/stream/live/([^/]+))", [&sessions](const httplib::Request &req, httplib::Response &res) {
    auto device_id = req.matches[1];
    auto params = parse_params(req);
    if (params.codec.empty()) params.codec = "mjpeg";
    if (params.container.empty()) params.container = "raw";

    // On-demand session creation.
    auto session = sessions.get_or_create(device_id, params);
    session->client_count.fetch_add(1);
    session->last_accessed = std::chrono::steady_clock::now();

    EffectiveParams eff{params, session->params};
    add_effective_headers(res, eff);

    // If request codec/container mismatch first-comer, respond 409 with Effective-Params.
    if (params.codec != session->params.codec || params.container != session->params.container) {
      res.status = 409;
      res.set_content(build_error_json("conflict", "params locked by first requester"), "application/json");
      session->client_count.fetch_sub(1);
      return;
    }

    // Start capture if needed (first requester defines params).
    if (!session->capture->running()) {
      if (!session->capture->start(device_id, session->params)) {
        res.status = 503;
      res.set_content(build_error_json("device_unavailable", "failed to open camera"), "application/json");
      session->client_count.fetch_sub(1);
      return;
    }
    session->pixel_format = session->capture->pixel_format();
    session->started = std::chrono::steady_clock::now();
    session->frames_sent = 0;
    session->bytes_sent = 0;
  }

    if (params.codec == "mjpeg") {
      serve_mjpeg_live(session->params, res, session);
    } else if (params.codec == "h264") {
      if (params.container == "mp4") {
        serve_fmp4_live(session->params, res, session);
      } else {
        serve_h264_live(session->params, res, session);
      }
    } else {
      res.status = 400;
      res.set_content(build_error_json("bad_request", "unsupported codec"), "application/json");
    }

    session->client_count.fetch_sub(1);
    sessions.release_if_idle(device_id);
  });

  svr.Get(R"(/stream/udp/([^/]+))", [&sessions](const httplib::Request &req, httplib::Response &res) {
#ifdef __linux__
    auto device_id = req.matches[1];
    if (!req.has_param("target") || !req.has_param("port")) {
      res.status = 400;
      res.set_content(build_error_json("bad_request", "target and port are required"), "application/json");
      return;
    }
    const std::string target = req.get_param_value("target");
    int port = std::stoi(req.get_param_value("port"));
    int duration_sec = req.has_param("duration") ? std::stoi(req.get_param_value("duration")) : 10;
    auto params = parse_params(req);
    if (params.codec.empty()) params.codec = "h264"; // UDP favors H.264

    auto session = sessions.get_or_create(device_id, params);
    session->client_count.fetch_add(1);
    session->last_accessed = std::chrono::steady_clock::now();

    if (!session->capture->running()) {
      if (!session->capture->start(device_id, session->params)) {
        res.status = 503;
        res.set_content(build_error_json("device_unavailable", "failed to open camera"), "application/json");
        session->client_count.fetch_sub(1);
        return;
      }
      session->pixel_format = session->capture->pixel_format();
      session->started = std::chrono::steady_clock::now();
      session->frames_sent = 0;
      session->bytes_sent = 0;
    }

    std::thread([session, params, target, port, duration_sec, &sessions]() {
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
      uint8_t* y = reinterpret_cast<uint8_t*>(yuv.data());
      uint8_t* u = y + y_size;
      uint8_t* v = u + uv_size;
      bool first = true;
      auto start = std::chrono::steady_clock::now();
      const int frame_interval_ms = std::max(1, 1000 / std::max(1, params.fps));
      const size_t mtu = 1400;

      while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= duration_sec) break;
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
            sendto(sock, frame.data() + offset, chunk, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            offset += chunk;
          }
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(frame.size());
        } else if (params.codec == "h264") {
#ifdef HAS_OPENH264
          if (!session->encoder) {
            session->encoder = std::make_shared<H264Encoder>();
            if (!session->encoder->init(session->params)) break;
            session->encoder->force_idr();
            first = false;
          }
          if (session->capture->pixel_format() != PixelFormat::YUYV) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          yuyv_to_i420(reinterpret_cast<const uint8_t*>(frame.data()), params.width, params.height, y, u, v);
          if (first) { session->encoder->force_idr(); first = false; }
          std::string nal;
          if (!session->encoder->encode_i420(y, u, v, nal)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          static const char start_code[] = {0x00, 0x00, 0x00, 0x01};
          std::string packet;
          packet.reserve(sizeof(start_code) + nal.size());
          packet.append(start_code, sizeof(start_code));
          packet.append(nal);
          sendto(sock, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(packet.size());
#else
          break;
#endif
        } else {
          break;
        }
        session->last_accessed = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
      }
      close(sock);
      session->client_count.fetch_sub(1);
      sessions.release_if_idle(session->device_id);
    }).detach();

    res.status = 200;
    res.set_content("{\"status\":\"udp_stream_started\"}", "application/json");
#else
    (void)req;
    res.status = 503;
    res.set_content(build_error_json("udp_unavailable", "UDP sender supported on Linux only"), "application/json");
#endif
  });

  svr.set_error_handler([](const httplib::Request &, httplib::Response &res) {
    if (res.status == 404) {
      res.set_content(build_error_json("not_found"), "application/json");
    } else {
      res.set_content(build_error_json("error"), "application/json");
    }
  });

  std::cout << "SilkCast server listening on " << cfg.addr << ":" << cfg.port
            << " (idle-timeout=" << cfg.idle_timeout << "s)" << std::endl;
  svr.listen(cfg.addr.c_str(), cfg.port);
  return 0;
}
