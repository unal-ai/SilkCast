#include "adapter_registry.hpp"

#include <algorithm>
#include <stdexcept>

namespace {
bool contains(const std::vector<std::string> &items, const std::string &value) {
  return std::find(items.begin(), items.end(), value) != items.end();
}

std::string json_array(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += "\"" + items[i] + "\"";
    if (i + 1 < items.size()) out += ",";
  }
  out += "]";
  return out;
}

std::string join_csv(const std::vector<std::string> &items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += ",";
    out += items[i];
  }
  return out;
}

std::string build_error_json(const std::string &msg, const std::string &details) {
  return "{\"error\":\"" + msg + "\",\"details\":\"" + details + "\"}";
}

std::string role_to_string(AdapterRole role) {
  return role == AdapterRole::Input ? "input" : "output";
}
} // namespace

const AdapterRegistry &AdapterRegistry::instance() {
  static const AdapterRegistry registry;
  return registry;
}

AdapterRegistry::AdapterRegistry() {
  register_media_kind("video", true);
  register_media_kind("audio", false);
  register_media_kind("av", false);

#ifdef __linux__
  const bool linux_enabled = true;
#else
  const bool linux_enabled = false;
#endif

#ifdef HAS_WEBSOCKET_TRANSPORT
  const bool websocket_enabled = true;
#else
  const bool websocket_enabled = false;
#endif

  register_adapter(
      {"v4l2_camera", AdapterRole::Input, "video", linux_enabled, {"live", "udp"}, {"linux"}, "V4L2 capture adapter"});
  register_adapter({"screen_mirror", AdapterRole::Input, "video", false, {"live"}, {"linux", "darwin", "windows"},
                    "Reserved screen mirror adapter"});
  register_adapter({"rtsp_ingest", AdapterRole::Input, "video", true, {"live"}, {"linux", "darwin", "windows"},
                    "RTSP ingest adapter"});
  register_adapter({"audio_stream_in", AdapterRole::Input, "audio", false, {"live"}, {"linux", "darwin", "windows"},
                    "Reserved audio ingest adapter"});

  register_adapter({"http_live_stream", AdapterRole::Output, "video", true, {"live"}, {"linux", "darwin", "windows"},
                    "HTTP live stream output"});
  register_adapter({"udp_stream", AdapterRole::Output, "video", linux_enabled, {"udp"}, {"linux"},
                    "UDP sender output"});
  register_adapter({"websocket_stream", AdapterRole::Output, "video", websocket_enabled, {"ws"},
                    {"linux", "darwin", "windows"},
                    websocket_enabled
                        ? "WebSocket output (binary frames over websocket sidecar listener)"
                        : "Reserved WebSocket output for WS-capable server build"});
  register_adapter({"virtual_camera", AdapterRole::Output, "video", false, {"virtual_camera"}, {"linux"},
                    "Reserved virtual camera output (v4l2loopback target)"});
  register_adapter({"virtual_microphone", AdapterRole::Output, "audio", false, {"virtual_microphone"}, {"linux"},
                    "Reserved virtual microphone output (loopback sink target)"});
}

void AdapterRegistry::register_media_kind(const std::string &media, bool enabled) {
  if (contains(known_media_kinds_, media)) {
    throw std::runtime_error("duplicate media registration: " + media);
  }
  known_media_kinds_.push_back(media);
  if (enabled) enabled_media_kinds_.push_back(media);
}

void AdapterRegistry::register_adapter(AdapterSpec spec) {
  if (!is_known_media_kind(spec.media)) {
    throw std::runtime_error("adapter '" + spec.name + "' references unknown media '" + spec.media + "'");
  }
  const std::string key = role_to_string(spec.role) + ":" + spec.name;
  if (adapter_keys_.find(key) != adapter_keys_.end()) {
    throw std::runtime_error("duplicate adapter registration: " + key);
  }
  adapter_keys_.insert(key);
  adapters_.push_back(std::move(spec));
}

bool AdapterRegistry::is_known_media_kind(const std::string &media) const {
  return contains(known_media_kinds_, media);
}

bool AdapterRegistry::is_enabled_media_kind(const std::string &media) const {
  return contains(enabled_media_kinds_, media);
}

bool AdapterRegistry::has_enabled_output_adapter(const std::string &media, const std::string &transport) const {
  for (const auto &adapter : adapters_) {
    if (adapter.role != AdapterRole::Output) continue;
    if (!adapter.enabled) continue;
    if (adapter.media != media) continue;
    if (contains(adapter.transports, transport)) return true;
  }
  return false;
}

AdapterValidationResult AdapterRegistry::validate_request(const std::string &media,
                                                          const std::string &transport) const {
  AdapterValidationResult result;

  if (!is_known_media_kind(media)) {
    result.ok = false;
    result.status = 400;
    result.body = build_error_json("bad_request",
                                   "unknown media '" + media + "', known=" + join_csv(known_media_kinds_));
    return result;
  }

  if (!is_enabled_media_kind(media)) {
    result.ok = false;
    result.status = 409;
    result.body =
        "{"
        "\"error\":\"unsupported_media\","
        "\"requested\":\"" + media + "\","
        "\"enabled\":" + json_array(enabled_media_kinds_) + ","
        "\"known\":" + json_array(known_media_kinds_) +
        "}";
    return result;
  }

  if (!has_enabled_output_adapter(media, transport)) {
    result.ok = false;
    result.status = 409;
    result.body = build_error_json(
        "unsupported_transport",
        "no enabled output adapter for media=" + media + " transport=" + transport);
    return result;
  }

  return result;
}

std::string AdapterRegistry::adapters_json() const {
  std::string out = "[";
  for (size_t i = 0; i < adapters_.size(); ++i) {
    const auto &a = adapters_[i];
    out += "{"
           "\"name\":\"" + a.name + "\","
           "\"role\":\"" + role_to_string(a.role) + "\","
           "\"media\":\"" + a.media + "\","
           "\"enabled\":" + std::string(a.enabled ? "true" : "false") + ","
           "\"transports\":" + json_array(a.transports) + ","
           "\"platforms\":" + json_array(a.platforms) + ","
           "\"details\":\"" + a.details + "\""
           "}";
    if (i + 1 < adapters_.size()) out += ",";
  }
  out += "]";
  return out;
}
