#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "types.hpp"

enum class TransportKind {
  Live,
  WebSocket,
  Udp
};

struct ValidationResult {
  bool ok = true;
  int status = 200;
  std::string body;
};

class CapabilityRegistry {
public:
  static const CapabilityRegistry &instance();

  const std::vector<std::string> &known_codecs() const { return known_codecs_; }
  const std::vector<std::string> &enabled_codecs() const { return enabled_codecs_; }
  const std::vector<std::string> &known_containers() const { return known_containers_; }

  bool is_known_codec(const std::string &codec) const;
  bool is_enabled_codec(const std::string &codec) const;
  bool is_known_container(const std::string &container) const;
  bool codec_supports_container(const std::string &codec, const std::string &container) const;

  ValidationResult validate(const CaptureParams &params, TransportKind transport) const;

private:
  struct CodecSpec {
    bool enabled = false;
    std::vector<std::string> containers;
  };

  CapabilityRegistry();
  void register_codec(const std::string &name, bool enabled, std::vector<std::string> containers);
  void register_container(const std::string &name);

  std::unordered_map<std::string, CodecSpec> codecs_;
  std::vector<std::string> known_codecs_;
  std::vector<std::string> enabled_codecs_;
  std::vector<std::string> known_containers_;
};
