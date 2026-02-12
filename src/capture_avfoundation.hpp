#pragma once

#include <string>
#include <vector>

#ifdef __APPLE__
std::vector<std::string> list_avfoundation_devices();
bool build_avfoundation_caps_json(const std::string &device_id,
                                  std::string &json_out,
                                  std::string &error_out);
#endif
