#pragma once

#include <string>
#include <unordered_set>
#include <vector>

enum class AdapterRole {
  Input,
  Output
};

struct AdapterSpec {
  std::string name;
  AdapterRole role = AdapterRole::Input;
  std::string media;
  bool enabled = false;
  std::vector<std::string> transports;
  std::vector<std::string> platforms;
  std::string details;
};

struct AdapterValidationResult {
  bool ok = true;
  int status = 200;
  std::string body;
};

class AdapterRegistry {
public:
  static const AdapterRegistry &instance();

  const std::vector<std::string> &known_media_kinds() const { return known_media_kinds_; }
  const std::vector<std::string> &enabled_media_kinds() const { return enabled_media_kinds_; }
  const std::vector<AdapterSpec> &adapters() const { return adapters_; }

  bool is_known_media_kind(const std::string &media) const;
  bool is_enabled_media_kind(const std::string &media) const;
  bool has_enabled_output_adapter(const std::string &media, const std::string &transport) const;

  AdapterValidationResult validate_request(const std::string &media, const std::string &transport) const;
  std::string adapters_json() const;

private:
  AdapterRegistry();

  void register_media_kind(const std::string &media, bool enabled);
  void register_adapter(AdapterSpec spec);

  std::vector<std::string> known_media_kinds_;
  std::vector<std::string> enabled_media_kinds_;
  std::vector<AdapterSpec> adapters_;
  std::unordered_set<std::string> adapter_keys_;
};
