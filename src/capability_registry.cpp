#include "capability_registry.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace {
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
  std::ostringstream oss;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) oss << ",";
    oss << items[i];
  }
  return oss.str();
}

bool contains(const std::vector<std::string> &items, const std::string &value) {
  return std::find(items.begin(), items.end(), value) != items.end();
}

std::string build_error_json(const std::string &msg, const std::string &details) {
  return "{\"error\":\"" + msg + "\",\"details\":\"" + details + "\"}";
}
} // namespace

const CapabilityRegistry &CapabilityRegistry::instance() {
  static const CapabilityRegistry registry;
  return registry;
}

CapabilityRegistry::CapabilityRegistry() {
  register_container("raw");
  register_container("mp4");

  register_codec("mjpeg", true, {"raw"});
#ifdef HAS_OPENH264
  register_codec("h264", true, {"raw", "mp4"});
#else
  register_codec("h264", false, {"raw", "mp4"});
#endif
  register_codec("h265", false, {"raw", "mp4"});
  register_codec("av1", false, {"raw", "mp4"});
}

void CapabilityRegistry::register_codec(const std::string &name,
                                        bool enabled,
                                        std::vector<std::string> containers) {
  if (codecs_.find(name) != codecs_.end()) {
    throw std::runtime_error("duplicate codec registration: " + name);
  }
  for (const auto &container : containers) {
    if (!is_known_container(container)) {
      throw std::runtime_error("codec '" + name + "' references unknown container '" + container + "'");
    }
  }
  codecs_.emplace(name, CodecSpec{enabled, std::move(containers)});
  known_codecs_.push_back(name);
  if (enabled) enabled_codecs_.push_back(name);
}

void CapabilityRegistry::register_container(const std::string &name) {
  if (contains(known_containers_, name)) {
    throw std::runtime_error("duplicate container registration: " + name);
  }
  known_containers_.push_back(name);
}

bool CapabilityRegistry::is_known_codec(const std::string &codec) const {
  return codecs_.find(codec) != codecs_.end();
}

bool CapabilityRegistry::is_enabled_codec(const std::string &codec) const {
  const auto it = codecs_.find(codec);
  if (it == codecs_.end()) return false;
  return it->second.enabled;
}

bool CapabilityRegistry::is_known_container(const std::string &container) const {
  return contains(known_containers_, container);
}

bool CapabilityRegistry::codec_supports_container(const std::string &codec,
                                                  const std::string &container) const {
  const auto it = codecs_.find(codec);
  if (it == codecs_.end()) return false;
  return contains(it->second.containers, container);
}

ValidationResult CapabilityRegistry::validate(const CaptureParams &params,
                                              TransportKind transport) const {
  ValidationResult result;

  if (!is_known_codec(params.codec)) {
    result.ok = false;
    result.status = 400;
    result.body = build_error_json(
        "bad_request",
        "unknown codec '" + params.codec + "', known=" + join_csv(known_codecs_));
    return result;
  }

  if (!is_enabled_codec(params.codec)) {
    result.ok = false;
    result.status = 409;
    result.body =
        "{"
        "\"error\":\"unsupported_codec\","
        "\"requested\":\"" + params.codec + "\","
        "\"enabled\":" + json_array(enabled_codecs_) + ","
        "\"known\":" + json_array(known_codecs_) +
        "}";
    return result;
  }

  if (!is_known_container(params.container)) {
    result.ok = false;
    result.status = 400;
    result.body =
        "{"
        "\"error\":\"unsupported_container\","
        "\"requested\":\"" + params.container + "\","
        "\"enabled\":" + json_array(known_containers_) +
        "}";
    return result;
  }

  if (!codec_supports_container(params.codec, params.container)) {
    result.ok = false;
    result.status = 409;
    result.body = build_error_json(
        "incompatible_params",
        "codec=" + params.codec + " does not support container=" + params.container);
    return result;
  }

  if (transport == TransportKind::Udp && params.container != "raw") {
    result.ok = false;
    result.status = 409;
    result.body = build_error_json("incompatible_params", "UDP transport requires container=raw");
    return result;
  }

  if (transport == TransportKind::WebSocket && params.container != "raw") {
    result.ok = false;
    result.status = 409;
    result.body = build_error_json("incompatible_params", "WebSocket transport requires container=raw");
    return result;
  }

  return result;
}
