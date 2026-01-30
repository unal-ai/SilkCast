#pragma once

#include <memory>
#include <string>
#include <vector>

#include "httplib.h"
#include "types.hpp"

class Session;

namespace stream {

// Misc helpers
std::string json_array(const std::vector<std::string> &items);
std::string build_error_json(const std::string &msg,
                             const std::string &details = "");
const char *pixel_format_label(PixelFormat fmt);

#ifdef __linux__
std::string build_device_caps_json(const std::string &device_id,
                                   std::string &error);
#endif

// Parameter parsing / syncing
CaptureParams parse_params(const httplib::Request &req);
void apply_latency_preset(CaptureParams &p);
void sync_session_params(Session &session);
void add_effective_headers(httplib::Response &res, const EffectiveParams &eff);

// Bitstream helpers
std::vector<uint8_t> annexb_to_avcc(const std::string &annexb);
void extract_sps_pps(const std::string &annexb, std::vector<uint8_t> &sps,
                     std::vector<uint8_t> &pps);

// Streaming responders
void serve_mjpeg_placeholder(const CaptureParams &p, httplib::Response &res,
                             std::shared_ptr<Session> session,
                             std::function<void(bool)> on_done);
void serve_mjpeg_live(const CaptureParams &p, httplib::Response &res,
                      std::shared_ptr<Session> session,
                      std::function<void(bool)> on_done);
void serve_h264_live(const CaptureParams &p, httplib::Response &res,
                     std::shared_ptr<Session> session,
                     std::function<void(bool)> on_done);
void serve_fmp4_live(const CaptureParams &p, httplib::Response &res,
                     std::shared_ptr<Session> session,
                     std::function<void(bool)> on_done);
bool preflight_fmp4_bootstrap(const CaptureParams &p,
                              std::shared_ptr<Session> session,
                              std::string &error);

} // namespace stream
