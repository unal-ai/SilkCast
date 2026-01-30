#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "api_router.hpp"
#include "capture_v4l2.hpp"
#include "client_pull.hpp"
#include "encoder_h264.hpp"
#include "httplib.h"
#include "index_html.hpp"
#include "mp4_frag.hpp"
#include "session_manager.hpp"
#include "stream_utils.hpp"
#include "types.hpp"
#include "yuv_convert.hpp"

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;

int main(int argc, char *argv[]) {
  struct Config {
    std::string addr = "0.0.0.0";
    int port = 8080;
    int idle_timeout = 10;
    std::string default_codec = "mjpeg";
    std::string connect_target = "";
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
    } else if (arg == "--connect" && i + 1 < argc) {
      cfg.connect_target = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "SilkCast\n"
                << "  --addr <ip>          Bind address (default 0.0.0.0)\n"
                << "  --port <port>        Bind port   (default 8080)\n"
                << "  --idle-timeout <s>   Idle seconds before closing device "
                   "(default 10)\n"
                << "  --codec <mjpeg|h264> Default codec if not specified "
                   "(default mjpeg)\n"
                << "  --connect <ip:port>  Run as client (pull stream from "
                   "server)\n";
      return 0;
    }
  }

  if (!cfg.connect_target.empty()) {
    return run_client(cfg.connect_target);
  }

  SessionManager sessions(cfg.idle_timeout);
  httplib::Server svr;
  ApiRouter api;

  // Streaming endpoints require chunked transfer encoding hacks and range
  // clearing.
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

  // Serve the dynamic index page.
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
                   res.set_content(stream::json_array(devices),
                                   "application/json");
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
         auto json = stream::build_device_caps_json(device_id, error);
         if (json.empty()) {
           res.status = 503;
           res.set_content(stream::build_error_json("caps_unavailable", error),
                           "application/json");
           return;
         }
         res.status = 200;
         res.set_header("Content-Type", "application/json");
         res.set_content(json, "application/json");
#else
         (void)device_id;
         res.status = 503;
         res.set_content(stream::build_error_json(
                             "caps_unavailable",
                             "device capabilities supported on Linux only"),
                         "application/json");
#endif
       }});

  // Stats route.
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
           res.set_content(stream::build_error_json(
                               "not_found", "device " + std::string(device_id)),
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
                             std::string(stream::pixel_format_label(
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
                             "\"quality\":" +
                             std::to_string(session->params.quality) +
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

  // Live stream route.
  api.add_route(
      {"/stream/live/{device}",
       "GET",
       "Start a live stream",
       {{"device", ParamType::Device, "video0", "Device ID"},
        {"w", ParamType::Int, "1280", "Width"},
        {"h", ParamType::Int, "720", "Height"},
        {"fps", ParamType::Int, "30", "Framerate"},
        {"bitrate", ParamType::Int, "256", "Bitrate (kbps)"},
        {"quality", ParamType::Int, "80", "JPEG quality (1-100, MJPEG only)"},
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
         auto params = stream::parse_params(req);
         if (params.codec.empty())
           params.codec = "mjpeg";
         if (params.container.empty())
           params.container = "raw";

         auto session = sessions.get_or_create(device_id, params);
         session->client_count.fetch_add(1);
         session->last_accessed = std::chrono::steady_clock::now();

         EffectiveParams eff{params, session->params};
         eff.actual.container = params.container;
         stream::add_effective_headers(res, eff);

         if (params.codec != session->params.codec) {
           res.status = 409;
           res.set_content(stream::build_error_json(
                               "conflict", "params locked by first requester"),
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
             res.set_content(stream::build_error_json("device_unavailable",
                                                      "failed to open camera"),
                             "application/json");
             session->client_count.fetch_sub(1);
             return;
           }
           stream::sync_session_params(*session);
           session->started = std::chrono::steady_clock::now();
           session->frames_sent = 0;
           session->bytes_sent = 0;
         }

         EffectiveParams eff_actual{params, session->params};
         eff_actual.actual.container = params.container;
         stream::add_effective_headers(res, eff_actual);

         if (params.container == "mp4" && params.codec != "h264") {
           res.status = 400;
           res.set_content(stream::build_error_json(
                               "bad_request", "mp4 container requires h264"),
                           "application/json");
           session->client_count.fetch_sub(1);
           sessions.release_if_idle(device_id);
           return;
         }

         if (params.codec == "mjpeg") {
           stream::serve_mjpeg_live(session->params, res, session, on_done);
         } else if (params.codec == "h264") {
           if (params.container == "mp4") {
             std::string error;
             if (!stream::preflight_fmp4_bootstrap(session->params, session,
                                                   error)) {
               res.status = 503;
               res.set_content(
                   stream::build_error_json("fmp4_unavailable", error),
                   "application/json");
               session->client_count.fetch_sub(1);
               sessions.release_if_idle(device_id);
               return;
             }
             stream::serve_fmp4_live(session->params, res, session, on_done);
           } else {
             stream::serve_h264_live(session->params, res, session, on_done);
           }
         } else {
           res.status = 400;
           res.set_content(
               stream::build_error_json("bad_request", "unsupported codec"),
               "application/json");
           session->client_count.fetch_sub(1);
           sessions.release_if_idle(device_id);
         }
       }});

  // UDP stream route.
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
        {"quality", ParamType::Int, "80", "JPEG quality (1-100, MJPEG only)"},
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
           res.set_content(stream::build_error_json(
                               "bad_request", "target and port are required"),
                           "application/json");
           return;
         }
         const std::string target = req.get_param_value("target");
         int port = std::stoi(req.get_param_value("port"));
         int duration_sec = req.has_param("duration")
                                ? std::stoi(req.get_param_value("duration"))
                                : 10;
         auto params = stream::parse_params(req);
         if (params.codec.empty())
           params.codec = "h264";

         auto session = sessions.get_or_create(device_id, params);
         session->client_count.fetch_add(1);
         session->last_accessed = std::chrono::steady_clock::now();

         if (!session->capture->running()) {
           if (!session->capture->start(device_id, session->params)) {
             res.status = 503;
             res.set_content(stream::build_error_json("device_unavailable",
                                                      "failed to open camera"),
                             "application/json");
             session->client_count.fetch_sub(1);
             return;
           }
           stream::sync_session_params(*session);
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
           uint32_t last_idr = session->idr_request_seq.load();
           const int frame_interval_ms =
               std::max(1, 1000 / std::max(1, params.fps));
           const size_t mtu = 1400;
           uint32_t frame_sequence = 0;

           while (true) {
             auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
             if (elapsed >= duration_sec)
               break;

             // Check for IDR requests
             uint32_t current_idr = session->idr_request_seq.load();
             if (current_idr > last_idr) {
#ifdef HAS_OPENH264
               if (encoder_ready) {
                 encoder.force_idr();
               }
#endif
               last_idr = current_idr;
             }

             if (!session->capture || !session->capture->running()) {
               std::this_thread::sleep_for(10ms);
               continue;
             }
             if (!session->capture->latest_frame(frame)) {
               std::this_thread::sleep_for(5ms);
               continue;
             }

             const uint8_t *p_data = nullptr;
             size_t p_size = 0;
             std::string h264_buf;

             if (params.codec == "mjpeg") {
               p_data = reinterpret_cast<const uint8_t *>(frame.data());
               p_size = frame.size();
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
               if (!nal.empty()) {
                 static const char start_code[] = {0x00, 0x00, 0x00, 0x01};
                 h264_buf.reserve(sizeof(start_code) + nal.size());
                 h264_buf.append(start_code, sizeof(start_code));
                 h264_buf.append(nal);
                 p_data = reinterpret_cast<const uint8_t *>(h264_buf.data());
                 p_size = h264_buf.size();
               }
#else
               break;
#endif
             } else {
               break;
             }

             if (p_size > 0) {
               size_t max_payload = mtu - sizeof(UdpFrameHeader);
               size_t offset = 0;
               uint16_t frag_id = 0;
               uint16_t num_frags = (p_size + max_payload - 1) / max_payload;

               while (offset < p_size) {
                 size_t chunk = std::min(max_payload, p_size - offset);
                 UdpFrameHeader header;
                 header.frame_id = frame_sequence;
                 header.frag_id = frag_id++;
                 header.num_frags = num_frags;
                 header.data_size = chunk;

                 std::vector<uint8_t> packet(sizeof(header) + chunk);
                 memcpy(packet.data(), &header, sizeof(header));
                 memcpy(packet.data() + sizeof(header), p_data + offset, chunk);

                 sendto(sock, packet.data(), packet.size(), 0,
                        reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

                 session->bytes_sent.fetch_add(packet.size());
                 offset += chunk;
               }
               session->frames_sent.fetch_add(1);
               frame_sequence++;
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
         res.set_content(
             stream::build_error_json("udp_unavailable",
                                      "UDP sender supported on Linux only"),
             "application/json");
#endif
       }});

  // Feedback route (IDR Request)
  api.add_route(
      {"/stream/{device}/feedback",
       "POST",
       "Send feedback (e.g. request IDR)",
       {{"device", ParamType::Device, "video0", "Device ID"},
        {"type", ParamType::Select, "idr", "Feedback Type", {"idr"}}},
       [&sessions](const httplib::Request &req, httplib::Response &res) {
         if (req.matches.size() < 2) {
           res.status = 404;
           return;
         }
         std::string device_id = req.matches[1].str();
         auto session_opt = sessions.find(device_id);
         if (!session_opt) {
           res.status = 404;
           res.set_content(
               stream::build_error_json("not_found", "session not active"),
               "application/json");
           return;
         }
         auto session = *session_opt;
         std::string type = req.get_param_value("type");
         if (type == "idr") {
           session->idr_request_seq.fetch_add(1);
           res.status = 200;
           res.set_content("{\"status\":\"idr_requested\"}",
                           "application/json");
         } else {
           res.status = 400;
           res.set_content(
               stream::build_error_json("bad_request", "unknown feedback type"),
               "application/json");
         }
       }});

  api.register_with(svr);

  std::cout << "SilkCast server listening on " << cfg.addr << ":" << cfg.port
            << " (idle-timeout=" << cfg.idle_timeout << "s)" << std::endl;
  svr.listen(cfg.addr.c_str(), cfg.port);
  return 0;
}
