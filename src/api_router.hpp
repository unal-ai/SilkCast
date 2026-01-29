#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "httplib.h"

// Simple JSON builder helper
inline std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[7];
        snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += c;
      }
    }
  }
  return out;
}

enum class ParamType { String, Int, Bool, Select, Device };

struct RouteParam {
  std::string name;
  ParamType type;
  std::string default_value;
  std::string description;
  std::vector<std::string> options; // For Select type

  std::string type_to_string() const {
    switch (type) {
    case ParamType::String:
      return "string";
    case ParamType::Int:
      return "int";
    case ParamType::Bool:
      return "bool";
    case ParamType::Select:
      return "select";
    case ParamType::Device:
      return "device";
    }
    return "string";
  }

  std::string to_json() const {
    std::stringstream ss;
    ss << "{";
    ss << "\"name\":\"" << json_escape(name) << "\",";
    ss << "\"type\":\"" << type_to_string() << "\",";
    ss << "\"default\":\"" << json_escape(default_value) << "\",";
    ss << "\"description\":\"" << json_escape(description) << "\"";
    if (!options.empty()) {
      ss << ",\"options\":[";
      for (size_t i = 0; i < options.size(); ++i) {
        ss << "\"" << json_escape(options[i]) << "\"";
        if (i < options.size() - 1)
          ss << ",";
      }
      ss << "]";
    }
    ss << "}";
    return ss.str();
  }
};

struct Route {
  std::string path;   // e.g., "/stream/live/{device}"
  std::string method; // GET, POST
  std::string description;
  std::vector<RouteParam> params;
  std::function<void(const httplib::Request &, httplib::Response &)> handler;

  std::string to_json() const {
    std::stringstream ss;
    ss << "{";
    ss << "\"path\":\"" << json_escape(path) << "\",";
    ss << "\"method\":\"" << json_escape(method) << "\",";
    ss << "\"description\":\"" << json_escape(description) << "\",";
    ss << "\"params\":[";
    for (size_t i = 0; i < params.size(); ++i) {
      ss << params[i].to_json();
      if (i < params.size() - 1)
        ss << ",";
    }
    ss << "]";
    ss << "}";
    return ss.str();
  }
};

class ApiRouter {
public:
  void add_route(const Route &route) { routes_.push_back(route); }

  // Register all routes with the actual httplib server
  void register_with(httplib::Server &svr) {
    std::cout << "[ApiRouter] Starting registration..." << std::endl;
    for (const auto &route : routes_) {
      // Convert /path/{param} to /path/([^/]+) for httplib
      std::string regex_path = route.path;
      size_t start_pos = 0;
      while ((start_pos = regex_path.find('{', start_pos)) !=
             std::string::npos) {
        size_t end_pos = regex_path.find('}', start_pos);
        if (end_pos != std::string::npos) {
          regex_path.replace(start_pos, end_pos - start_pos + 1, "([^/]+)");
          start_pos += 8; // length of "([^/]+)"
        } else {
          break;
        }
      }

      std::cout << "[ApiRouter] Registering route: " << route.method << " "
                << regex_path << " (orig: " << route.path << ")" << std::endl;

      if (route.method == "GET") {
        svr.Get(regex_path, route.handler);
      } else if (route.method == "POST") {
        svr.Post(regex_path, route.handler);
      }
    }

    // Add schema endpoint
    std::cout << "[ApiRouter] Registering /api/schema" << std::endl;
    svr.Get("/api/schema",
            [this](const httplib::Request &, httplib::Response &res) {
              std::cout << "[ApiRouter] Serving /api/schema" << std::endl;
              res.set_header("Access-Control-Allow-Origin", "*");
              res.set_content(this->get_schema_json(), "application/json");
            });
  }

  std::string get_schema_json() const {
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < routes_.size(); ++i) {
      ss << routes_[i].to_json();
      if (i < routes_.size() - 1)
        ss << ",";
    }
    ss << "]";
    return ss.str();
  }

private:
  std::vector<Route> routes_;
};
