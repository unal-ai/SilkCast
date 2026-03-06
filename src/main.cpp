#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>

#include "api_router.hpp"
#include "adapter_registry.hpp"
#include "capability_registry.hpp"
#include "capture_avfoundation.hpp"
#include "capture_v4l2.hpp"
#include "client_pull.hpp"
#include "encoder_h264.hpp"
#include "httplib.h"
#include "index_html.hpp"
#include "mp4_frag.hpp"
#include "session_manager.hpp"
#include "stream_utils.hpp"
#include "types.hpp"
#include "ws_server.hpp"
#include "yuv_convert.hpp"

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;

namespace {
std::string to_lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string host_without_port(const std::string &host_header) {
  if (host_header.empty()) {
    return stream::get_local_ip_address();
  }
  if (host_header.front() == '[') {
    const auto close = host_header.find(']');
    if (close != std::string::npos) {
      return host_header.substr(0, close + 1);
    }
  }
  const auto colon = host_header.find(':');
  if (colon == std::string::npos) {
    return host_header;
  }
  return host_header.substr(0, colon);
}

std::string url_encode(const std::string &value) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    const bool safe =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0x0F]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

bool validate_media_params(const AdapterRegistry &adapters,
                           const std::string &media,
                           const std::string &transport,
                           httplib::Response &res) {
  const auto vr = adapters.validate_request(media, transport);
  if (vr.ok) return true;
  res.status = vr.status;
  res.set_content(vr.body, "application/json");
  return false;
}

bool validate_transport_params(const CapabilityRegistry &registry,
                               const CaptureParams &params,
                               TransportKind transport,
                               httplib::Response &res) {
  const auto vr = registry.validate(params, transport);
  if (vr.ok) return true;
  res.status = vr.status;
  res.set_content(vr.body, "application/json");
  return false;
}
} // namespace

int main(int argc, char *argv[]) {
  struct Config {
    std::string addr = "0.0.0.0";
    int port = 8080;
    int ws_port = -1;
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
    } else if (arg == "--ws-port" && i + 1 < argc) {
      cfg.ws_port = std::stoi(argv[++i]);
    } else if (arg == "--idle-timeout" && i + 1 < argc) {
      cfg.idle_timeout = std::stoi(argv[++i]);
    } else if (arg == "--codec" && i + 1 < argc) {
      cfg.default_codec = to_lower_copy(std::string(argv[++i]));
    } else if (arg == "--connect" && i + 1 < argc) {
      cfg.connect_target = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "SilkCast\n"
                << "  --addr <ip>          Bind address (default 0.0.0.0)\n"
                << "  --port <port>        Bind port   (default 8080)\n"
                << "  --ws-port <port>     WebSocket sidecar port "
                   "(default port+1, 0 disables)\n"
                << "  --idle-timeout <s>   Idle seconds before closing device "
                   "(default 10)\n"
                << "  --codec <mjpeg|h264|raw> Default codec if not specified "
                   "(default mjpeg)\n"
                << "  --connect <ip:port>  Run as client (pull stream from "
                   "server)\n";
      return 0;
    }
  }

  if (!cfg.connect_target.empty()) {
    return run_client(cfg.connect_target);
  }

  cfg.default_codec = to_lower_copy(cfg.default_codec);
  const auto &registry = CapabilityRegistry::instance();
  const auto &adapters = AdapterRegistry::instance();
  if (!registry.is_known_codec(cfg.default_codec) ||
      !registry.is_enabled_codec(cfg.default_codec)) {
    std::cerr << "default codec '" << cfg.default_codec
              << "' is not enabled in this build; falling back to 'mjpeg'\n";
    cfg.default_codec = "mjpeg";
  }
  if (cfg.ws_port < 0) {
    cfg.ws_port = cfg.port + 1;
  }
  if (cfg.ws_port == cfg.port) {
    std::cerr << "ws-port cannot equal http port; disabling websocket sidecar\n";
    cfg.ws_port = 0;
  }

  SessionManager sessions(cfg.idle_timeout);
  WebSocketServer ws_server(
      sessions, registry, adapters,
      WsServerConfig{cfg.addr, cfg.ws_port, cfg.default_codec});
  bool ws_enabled = false;
  if (cfg.ws_port > 0) {
    ws_enabled = ws_server.start();
    if (!ws_enabled) {
      std::cerr << "websocket sidecar failed to start on port " << cfg.ws_port
                << std::endl;
    }
  }

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

  api.add_route({"/capabilities",
                 "GET",
                 "Capability snapshot from strong registries",
                 {},
                 [&registry, &adapters](const httplib::Request &,
                                        httplib::Response &res) {
                   res.status = 200;
                   res.set_content(
                       "{"
                       "\"codecs\":{"
                       "\"known\":" +
                           stream::json_array(registry.known_codecs()) + ","
                       "\"enabled\":" +
                           stream::json_array(registry.enabled_codecs()) +
                           "},"
                       "\"containers\":{"
                       "\"known\":" +
                           stream::json_array(registry.known_containers()) +
                           "},"
                       "\"raw_formats\":{"
                       "\"enabled\":[\"i420\",\"rgb24\"]"
                       "},"
                       "\"media\":{"
                       "\"known\":" +
                           stream::json_array(adapters.known_media_kinds()) +
                           ","
                       "\"enabled\":" +
                           stream::json_array(adapters.enabled_media_kinds()) +
                           "},"
                       "\"adapters\":" +
                           adapters.adapters_json() + "}",
                       "application/json");
                 }});

  api.add_route({"/system/info",
                 "GET",
                 "Get basic server info",
                 {},
                 [&cfg, ws_enabled](const httplib::Request &,
                                    httplib::Response &res) {
                   res.status = 200;
                   res.set_content(
                       "{"
                       "\"ip\":\"" + stream::get_local_ip_address() + "\","
                       "\"port\":" + std::to_string(cfg.port) + ","
                       "\"ws_port\":" + std::to_string(cfg.ws_port) + ","
                       "\"ws_enabled\":" +
                           std::string(ws_enabled ? "true" : "false") + ","
                       "\"version\":\"1.0\""
                       "}",
                       "application/json");
                 }});

  const auto ws_upgrade_hint = [&cfg, ws_enabled](const std::string &target,
                                                  const httplib::Request &req,
                                                  httplib::Response &res) {
    if (!ws_enabled) {
      res.status = 503;
      res.set_content(
          stream::build_error_json(
              "websocket_unavailable",
              "websocket sidecar failed to start; check ws-port"),
          "application/json");
      return;
    }
    const std::string host = host_without_port(req.get_header_value("Host"));
    const std::string ws_url =
        "ws://" + host + ":" + std::to_string(cfg.ws_port) + target;
    res.status = 426;
    res.set_header("Upgrade", "websocket");
    res.set_content(
        "{"
        "\"error\":\"upgrade_required\","
        "\"details\":\"connect using a websocket client\","
        "\"ws_url\":\"" +
            json_escape(ws_url) + "\""
            "}",
        "application/json");
  };

  const auto append_ws_query_params = [](const httplib::Request &req,
                                         std::string &ws_target) {
    bool first = true;
    const auto add_param = [&req, &ws_target, &first](const char *name) {
      if (!req.has_param(name)) {
        return;
      }
      ws_target += first ? "?" : "&";
      first = false;
      ws_target += name;
      ws_target += "=";
      ws_target += url_encode(req.get_param_value(name));
    };
    add_param("media");
    add_param("w");
    add_param("h");
    add_param("fps");
    add_param("bitrate");
    add_param("quality");
    add_param("gop");
    add_param("codec");
    add_param("latency");
    add_param("container");
    add_param("pixfmt");
  };

  api.add_route({"/stream/ws/{device}",
                 "GET",
                 "WebSocket stream endpoint (upgrade hint over HTTP)",
                 {{"device", ParamType::Device, "video0", "Device ID"}},
                 [&ws_upgrade_hint, &append_ws_query_params](
                     const httplib::Request &req, httplib::Response &res) {
                   std::string ws_target = req.path;
                   append_ws_query_params(req, ws_target);
                   ws_upgrade_hint(ws_target, req, res);
                 }});
  api.add_route({"/stream/ws",
                 "GET",
                 "WebSocket stream endpoint (query id form)",
                 {{"id", ParamType::Device, "video0", "Device ID"}},
                 [&ws_upgrade_hint, &append_ws_query_params](
                     const httplib::Request &req, httplib::Response &res) {
                   if (!req.has_param("id") || req.get_param_value("id").empty()) {
                     res.status = 400;
                     res.set_content(
                         stream::build_error_json("bad_request",
                                                  "id query parameter is required"),
                         "application/json");
                     return;
                   }
                   std::string ws_target =
                       "/stream/ws/" + url_encode(req.get_param_value("id"));
                   append_ws_query_params(req, ws_target);
                   ws_upgrade_hint(ws_target, req, res);
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
         std::string device_id = stream::url_decode(req.matches[1].str());
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
#elif defined(__APPLE__)
         std::string json;
         std::string error;
         if (!build_avfoundation_caps_json(device_id, json, error)) {
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
         std::string device_id = stream::url_decode(req.matches[1].str());
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
         const auto state = session->state.load();
         const auto reason = session->teardown_reason.load();

         res.status = 200;
         res.set_content("{"
                         "\"device\":\"" +
                             session->device_id +
                             "\","
                             "\"codec\":\"" +
                             session->params.codec +
                             "\","
                             "\"pixfmt\":\"" +
                             (session->params.pixfmt.empty()
                                  ? std::string("")
                                  : session->params.pixfmt) +
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
                             "\"state\":\"" +
                             std::string(session_state_label(state)) +
                             "\","
                             "\"teardown_reason\":\"" +
                             std::string(teardown_reason_label(reason)) +
                             "\","
                             "\"active_clients\":" +
                             std::to_string(session->client_count.load()) +
                             ","
                             "\"startup_ms\":" +
                             std::to_string(session->startup_ms.load()) +
                             ","
                             "\"first_frame_ms\":" +
                             std::to_string(session->first_frame_ms.load()) +
                             ","
                             "\"first_iframe_ms\":" +
                             std::to_string(session->first_iframe_ms.load()) +
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
        {"media",
         ParamType::Select,
         "video",
         "Media Kind",
         {"video", "audio", "av"}},
        {"w", ParamType::Int, "1280", "Width"},
        {"h", ParamType::Int, "720", "Height"},
        {"fps", ParamType::Int, "30", "Framerate"},
        {"bitrate", ParamType::Int, "256", "Bitrate (kbps)"},
        {"quality", ParamType::Int, "80", "JPEG quality (1-100, MJPEG only)"},
        {"gop", ParamType::Int, "30", "GOP Size"},
        {"codec",
         ParamType::Select,
         "mjpeg",
         "Video Codec",
         {"mjpeg", "h264", "raw", "h265", "av1"}},
        {"latency",
         ParamType::Select,
         "view",
         "Latency Mode",
         {"view", "low", "ultra"}},
        {"container",
         ParamType::Select,
         "raw",
         "Container Format",
         {"raw", "mp4"}},
        {"pixfmt",
         ParamType::Select,
         "i420",
         "Raw Pixel Format (codec=raw only)",
         {"i420", "rgb24"}}},
       [&sessions, &cfg, &registry, &adapters](const httplib::Request &req,
                                                httplib::Response &res) {
         if (req.matches.size() < 2) {
           res.status = 404;
           return;
         }
         std::string device_id = stream::url_decode(req.matches[1].str());
         bool is_rtsp = device_id.rfind("rtsp://", 0) == 0;
         CaptureParams params;
         std::string parse_error_json;
         if (!stream::parse_params(req, params, parse_error_json)) {
           res.status = 400;
           res.set_content(parse_error_json, "application/json");
           return;
         }
         if (!req.has_param("media"))
           params.media = "video";
         if (!req.has_param("codec"))
           params.codec = is_rtsp ? "h264" : cfg.default_codec;
         if (!req.has_param("container"))
           params.container = is_rtsp ? "mp4" : "raw";
         if (!req.has_param("latency") && is_rtsp)
           params.latency = "ultra";
         if (params.codec == "raw") {
           if (params.pixfmt.empty()) {
             params.pixfmt = "i420";
           }
         } else {
           params.pixfmt.clear();
         }
         if (is_rtsp && params.codec == "raw") {
           res.status = 409;
           res.set_content(stream::build_error_json(
                               "incompatible_params",
                               "rtsp sources do not expose raw frames"),
                           "application/json");
           return;
         }
         if (!validate_media_params(adapters, params.media, "live", res))
           return;
         if (!validate_transport_params(registry, params, TransportKind::Live,
                                        res))
           return;

         auto session = sessions.get_or_create(device_id, params);
         if (params.media != session->params.media ||
             params.codec != session->params.codec ||
             params.container != session->params.container ||
             params.pixfmt != session->params.pixfmt ||
             params.width != session->params.width ||
             params.height != session->params.height ||
             params.fps != session->params.fps ||
             params.bitrate_kbps != session->params.bitrate_kbps ||
             params.quality != session->params.quality ||
             params.gop != session->params.gop ||
             params.latency != session->params.latency) {
           res.status = 409;
           res.set_content(
               stream::build_error_json("conflict",
                                        "params locked by first requester"),
               "application/json");
           return;
         }
         session->client_count.fetch_add(1);
         session->last_accessed = std::chrono::steady_clock::now();
         session->state.store(SessionState::Live);
         session->teardown_reason.store(TeardownReason::None);

         auto detach_client = [session, device_id, &sessions]() {
           const int prev = session->client_count.fetch_sub(1);
           if (prev <= 1) {
             session->client_count.store(0);
           }
           sessions.release_if_idle(device_id);
         };

         auto on_done = [detach_client](bool) {
           detach_client();
         };

         if (!session->capture->running()) {
           session->state.store(SessionState::Warming);
           const auto warming_started_at = std::chrono::steady_clock::now();
           if (!session->capture->start(device_id, session->params)) {
             res.status = 503;
             res.set_content(stream::build_error_json("device_unavailable",
                                                      "failed to open camera"),
                             "application/json");
             session->state.store(SessionState::Idle);
             session->teardown_reason.store(TeardownReason::OpenFailed);
             detach_client();
             return;
           }
           stream::sync_session_params(*session);
           session->started = std::chrono::steady_clock::now();
           session->frames_sent = 0;
           session->bytes_sent = 0;
           session->startup_ms.store(static_cast<uint64_t>(
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   session->started - warming_started_at)
                   .count()));
           session->first_frame_ms.store(0);
           session->first_frame_marked.store(false);
           session->first_iframe_ms.store(0);
           session->first_iframe_marked.store(false);
           session->state.store(SessionState::Live);
           session->teardown_reason.store(TeardownReason::None);
         }

         CaptureParams effective_output = session->params;
         if (effective_output.codec == "h264" && params.container == "mp4") {
           effective_output.container = "mp4";
         } else {
           effective_output.container = "raw";
         }
         EffectiveParams eff_actual{params, effective_output};
         stream::add_effective_headers(res, eff_actual);

         if (effective_output.codec == "mjpeg") {
           stream::serve_mjpeg_live(effective_output, res, session, on_done);
         } else if (effective_output.codec == "raw") {
           stream::serve_raw_live(effective_output, res, session, on_done);
         } else if (effective_output.codec == "h264") {
           if (effective_output.container == "mp4") {
             std::string error;
             if (!stream::preflight_fmp4_bootstrap(effective_output, session,
                                                   error)) {
               res.status = 503;
               res.set_content(
                   stream::build_error_json("fmp4_unavailable", error),
                   "application/json");
               session->teardown_reason.store(TeardownReason::RuntimeError);
               detach_client();
               return;
             }
             stream::serve_fmp4_live(effective_output, res, session, on_done);
           } else {
             stream::serve_h264_live(effective_output, res, session, on_done);
           }
         } else {
           res.status = 500;
           res.set_content(stream::build_error_json(
                               "internal_error",
                               "validated codec missing runtime handler"),
                           "application/json");
           session->teardown_reason.store(TeardownReason::RuntimeError);
           detach_client();
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
       [&sessions, &registry, &adapters](const httplib::Request &req,
                                         httplib::Response &res) {
#ifdef __linux__
         if (req.matches.size() < 2) {
           res.status = 404;
           return;
         }
         std::string device_id = stream::url_decode(req.matches[1].str());
         if (!req.has_param("target") || !req.has_param("port")) {
           res.status = 400;
           res.set_content(stream::build_error_json(
                               "bad_request", "target and port are required"),
                           "application/json");
           return;
         }
         const std::string target = req.get_param_value("target");
         int port = 0;
         int duration_sec = 10;
         try {
           port = std::stoi(req.get_param_value("port"));
           if (req.has_param("duration")) {
             duration_sec = std::stoi(req.get_param_value("duration"));
           }
         } catch (...) {
           res.status = 400;
           res.set_content(
               stream::build_error_json("bad_request",
                                        "port and duration must be integers"),
               "application/json");
           return;
         }
         if (port <= 0 || port > 65535 || duration_sec <= 0) {
           res.status = 400;
           res.set_content(
               stream::build_error_json("bad_request",
                                        "port must be 1-65535 and duration > 0"),
               "application/json");
           return;
         }
         CaptureParams params;
         std::string parse_error_json;
         if (!stream::parse_params(req, params, parse_error_json)) {
           res.status = 400;
           res.set_content(parse_error_json, "application/json");
           return;
         }
         if (!req.has_param("media"))
           params.media = "video";
         if (!req.has_param("codec"))
           params.codec = "h264";
         if (!req.has_param("container"))
           params.container = "raw";
         if (params.codec == "raw") {
           res.status = 409;
           res.set_content(stream::build_error_json(
                               "incompatible_params",
                               "udp transport does not expose raw frames yet"),
                           "application/json");
           return;
         }
         if (!validate_media_params(adapters, params.media, "udp", res))
           return;
         if (!validate_transport_params(registry, params, TransportKind::Udp,
                                        res))
           return;

         auto session = sessions.get_or_create(device_id, params);
         session->client_count.fetch_add(1);
         session->last_accessed = std::chrono::steady_clock::now();
         session->state.store(SessionState::Live);
         session->teardown_reason.store(TeardownReason::None);

         auto detach_client = [session, device_id, &sessions]() {
           const int prev = session->client_count.fetch_sub(1);
           if (prev <= 1) {
             session->client_count.store(0);
           }
           sessions.release_if_idle(device_id);
         };

         if (params.media != session->params.media ||
             params.codec != session->params.codec ||
             params.container != session->params.container) {
           res.status = 409;
           res.set_content(stream::build_error_json(
                               "conflict", "params locked by first requester"),
                           "application/json");
           detach_client();
           return;
         }

         if (!session->capture->running()) {
           session->state.store(SessionState::Warming);
           const auto warming_started_at = std::chrono::steady_clock::now();
           if (!session->capture->start(device_id, session->params)) {
             res.status = 503;
             res.set_content(stream::build_error_json("device_unavailable",
                                                      "failed to open camera"),
                             "application/json");
             session->state.store(SessionState::Idle);
             session->teardown_reason.store(TeardownReason::OpenFailed);
             detach_client();
             return;
           }
           stream::sync_session_params(*session);
           session->started = std::chrono::steady_clock::now();
           session->frames_sent = 0;
           session->bytes_sent = 0;
           session->startup_ms.store(static_cast<uint64_t>(
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   session->started - warming_started_at)
                   .count()));
           session->first_frame_ms.store(0);
           session->first_frame_marked.store(false);
           session->first_iframe_ms.store(0);
           session->first_iframe_marked.store(false);
           session->state.store(SessionState::Live);
           session->teardown_reason.store(TeardownReason::None);
         }

         std::thread([session, params, target, port, duration_sec,
                      detach_client]() {
           int sock = socket(AF_INET, SOCK_DGRAM, 0);
           if (sock < 0) {
             session->teardown_reason.store(TeardownReason::RuntimeError);
             detach_client();
             return;
           }
           sockaddr_in addr{};
           addr.sin_family = AF_INET;
           addr.sin_port = htons(port);
           if (inet_pton(AF_INET, target.c_str(), &addr.sin_addr) != 1) {
             close(sock);
             session->teardown_reason.store(TeardownReason::RuntimeError);
             detach_client();
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
               stream::note_frame_sent(*session, 0);
               frame_sequence++;
             }
             std::this_thread::sleep_for(
                 std::chrono::milliseconds(frame_interval_ms));
           }
           close(sock);
           detach_client();
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
         std::string device_id = stream::url_decode(req.matches[1].str());
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

  std::cout << "SilkCast server detected IP: " << stream::get_local_ip_address()
            << std::endl;
  std::cout << "SilkCast server listening on " << cfg.addr << ":" << cfg.port
            << " (idle-timeout=" << cfg.idle_timeout << "s)" << std::endl;
  svr.listen(cfg.addr.c_str(), cfg.port);
  return 0;
}
