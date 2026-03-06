#include "ws_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adapter_registry.hpp"
#include "api_router.hpp"
#include "capability_registry.hpp"
#include "encoder_h264.hpp"
#include "session_manager.hpp"
#include "stream_utils.hpp"
#include "types.hpp"
#include "yuv_convert.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {
#if defined(__linux__) || defined(__APPLE__)

struct HttpRequest {
  std::string method;
  std::string target;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
};

using QueryMap = std::unordered_map<std::string, std::string>;

std::string to_lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string status_text(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 409:
    return "Conflict";
  case 426:
    return "Upgrade Required";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  default:
    return "Error";
  }
}

bool send_all(int fd, const void *data, size_t len) {
  const char *p = reinterpret_cast<const char *>(data);
  size_t sent = 0;
  while (sent < len) {
    const ssize_t rc = send(fd, p + sent, len - sent, 0);
    if (rc <= 0) {
      return false;
    }
    sent += static_cast<size_t>(rc);
  }
  return true;
}

bool send_http_json(int fd, int status, const std::string &body,
                    const std::vector<std::pair<std::string, std::string>>
                        &extra_headers = {}) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << " " << status_text(status) << "\r\n";
  for (const auto &kv : extra_headers) {
    oss << kv.first << ": " << kv.second << "\r\n";
  }
  oss << "Content-Type: application/json\r\n";
  oss << "Content-Length: " << body.size() << "\r\n";
  oss << "Connection: close\r\n\r\n";
  const std::string headers = oss.str();
  if (!send_all(fd, headers.data(), headers.size())) {
    return false;
  }
  if (!body.empty()) {
    return send_all(fd, body.data(), body.size());
  }
  return true;
}

bool read_http_request(int fd, HttpRequest &request) {
  std::string buffer;
  buffer.reserve(4096);
  char chunk[1024];
  constexpr size_t kMaxHeaders = 64 * 1024;

  while (buffer.find("\r\n\r\n") == std::string::npos) {
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(n));
    if (buffer.size() > kMaxHeaders) {
      return false;
    }
  }

  const size_t headers_end = buffer.find("\r\n\r\n");
  std::string header_block = buffer.substr(0, headers_end);
  std::istringstream input(header_block);
  std::string line;
  if (!std::getline(input, line)) {
    return false;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  std::istringstream first_line(line);
  first_line >> request.method >> request.target >> request.version;
  if (request.method.empty() || request.target.empty()) {
    return false;
  }

  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const auto sep = line.find(':');
    if (sep == std::string::npos) {
      continue;
    }
    std::string key = to_lower_copy(line.substr(0, sep));
    std::string value = line.substr(sep + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value[0]))) {
      value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
      value.pop_back();
    }
    request.headers[key] = value;
  }

  return true;
}

std::pair<std::string, QueryMap> split_target(const std::string &target) {
  QueryMap query;
  const auto qpos = target.find('?');
  const std::string path = (qpos == std::string::npos)
                               ? target
                               : target.substr(0, qpos);
  if (qpos == std::string::npos || qpos + 1 >= target.size()) {
    return {path, query};
  }
  const std::string query_str = target.substr(qpos + 1);
  size_t start = 0;
  while (start < query_str.size()) {
    const size_t end = query_str.find('&', start);
    const std::string token = query_str.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!token.empty()) {
      const size_t eq = token.find('=');
      if (eq == std::string::npos) {
        query[stream::url_decode(token)] = "";
      } else {
        query[stream::url_decode(token.substr(0, eq))] =
            stream::url_decode(token.substr(eq + 1));
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return {path, query};
}

std::string get_header(const HttpRequest &request, const std::string &name) {
  const auto it = request.headers.find(to_lower_copy(name));
  if (it == request.headers.end()) {
    return "";
  }
  return it->second;
}

bool header_contains_token(const HttpRequest &request, const std::string &name,
                           const std::string &token) {
  const auto value = to_lower_copy(get_header(request, name));
  return value.find(to_lower_copy(token)) != std::string::npos;
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

std::string ws_hint_url(const HttpRequest &request, int ws_port,
                        const std::string &target) {
  return "ws://" + host_without_port(get_header(request, "host")) + ":" +
         std::to_string(ws_port) + target;
}

bool is_supported_ws_path(const std::string &path) {
  return path == "/stream/ws" || path.rfind("/stream/ws/", 0) == 0;
}

std::optional<std::string> extract_device_id(const std::string &path,
                                             const QueryMap &query,
                                             std::string &error_json) {
  constexpr const char *kPrefix = "/stream/ws/";
  if (path.rfind(kPrefix, 0) == 0) {
    const std::string raw = path.substr(std::strlen(kPrefix));
    if (raw.empty()) {
      error_json = stream::build_error_json("bad_request", "device id is required");
      return std::nullopt;
    }
    return stream::url_decode(raw);
  }
  if (path == "/stream/ws") {
    const auto it = query.find("id");
    if (it == query.end() || it->second.empty()) {
      error_json = stream::build_error_json("bad_request",
                                            "id query parameter is required");
      return std::nullopt;
    }
    return stream::url_decode(it->second);
  }
  error_json = stream::build_error_json("not_found", "unknown websocket route");
  return std::nullopt;
}

std::string build_param_error_json(const std::string &field,
                                   const std::string &value,
                                   const std::string &expected,
                                   const std::string &details) {
  return "{"
         "\"error\":\"bad_request\","
         "\"details\":\"" +
         json_escape(details) +
         "\","
         "\"field\":\"" +
         json_escape(field) +
         "\","
         "\"value\":\"" +
         json_escape(value) +
         "\","
         "\"expected\":\"" +
         json_escape(expected) + "\""
         "}";
}

bool parse_int_param(const QueryMap &query, const char *name, int min_value,
                     int max_value, int &out, std::string &error_json) {
  const auto it = query.find(name);
  if (it == query.end()) {
    return true;
  }
  const std::string &value = it->second;
  try {
    size_t pos = 0;
    const long long parsed = std::stoll(value, &pos, 10);
    if (pos != value.size()) {
      error_json = build_param_error_json(
          name, value, "integer",
          std::string("parameter '") + name + "' must be an integer");
      return false;
    }
    if (parsed < min_value || parsed > max_value) {
      error_json = build_param_error_json(
          name, value,
          std::string("integer in [") + std::to_string(min_value) + "," +
              std::to_string(max_value) + "]",
          std::string("parameter '") + name + "' is out of range");
      return false;
    }
    out = static_cast<int>(parsed);
    return true;
  } catch (...) {
    error_json = build_param_error_json(
        name, value, "integer",
        std::string("parameter '") + name + "' must be an integer");
    return false;
  }
}

bool parse_capture_params(const QueryMap &query, CaptureParams &out,
                          std::string &error_json) {
  CaptureParams params;
  if (!parse_int_param(query, "w", 1, 16384, params.width, error_json)) {
    return false;
  }
  if (!parse_int_param(query, "h", 1, 16384, params.height, error_json)) {
    return false;
  }
  if (!parse_int_param(query, "fps", 1, 240, params.fps, error_json)) {
    return false;
  }
  if (!parse_int_param(query, "bitrate", 1, 1000000, params.bitrate_kbps,
                       error_json)) {
    return false;
  }
  if (!parse_int_param(query, "quality", 1, 100, params.quality, error_json)) {
    return false;
  }
  if (!parse_int_param(query, "gop", 1, 1000, params.gop, error_json)) {
    return false;
  }

  const auto media_it = query.find("media");
  if (media_it != query.end()) {
    params.media = media_it->second;
  }
  const auto codec_it = query.find("codec");
  if (codec_it != query.end()) {
    params.codec = codec_it->second;
  }
  const auto latency_it = query.find("latency");
  if (latency_it != query.end()) {
    params.latency = latency_it->second;
  }
  const auto container_it = query.find("container");
  if (container_it != query.end()) {
    params.container = container_it->second;
  }
  const auto pixfmt_it = query.find("pixfmt");
  if (pixfmt_it != query.end()) {
    params.pixfmt = pixfmt_it->second;
  }

  params.media = to_lower_copy(params.media);
  params.codec = to_lower_copy(params.codec);
  params.latency = to_lower_copy(params.latency);
  params.container = to_lower_copy(params.container);
  params.pixfmt = to_lower_copy(params.pixfmt);
  stream::apply_latency_preset(params);
  if (params.codec == "raw") {
    if (params.pixfmt.empty()) {
      params.pixfmt = "i420";
    } else if (!stream::is_supported_raw_pixfmt(params.pixfmt)) {
      error_json = build_param_error_json(
          "pixfmt", params.pixfmt, "i420|rgb24",
          "parameter 'pixfmt' must be one of: i420, rgb24");
      return false;
    }
  }
  out = std::move(params);
  return true;
}

bool contains_idr(const std::string &bitstream) {
  if (bitstream.empty()) {
    return false;
  }
  auto is_start_code = [&bitstream](size_t pos) {
    return pos + 2 < bitstream.size() && bitstream[pos] == 0 &&
           bitstream[pos + 1] == 0 &&
           (bitstream[pos + 2] == 1 ||
            (bitstream[pos + 2] == 0 && pos + 3 < bitstream.size() &&
             bitstream[pos + 3] == 1));
  };

  bool saw_start_code = false;
  for (size_t i = 0; i + 3 < bitstream.size(); ++i) {
    if (!is_start_code(i)) {
      continue;
    }
    saw_start_code = true;
    const size_t sc_size = bitstream[i + 2] == 1 ? 3 : 4;
    const size_t nal_pos = i + sc_size;
    if (nal_pos >= bitstream.size()) {
      break;
    }
    const uint8_t nal_type =
        static_cast<uint8_t>(bitstream[nal_pos]) & static_cast<uint8_t>(0x1f);
    if (nal_type == 5) {
      return true;
    }
  }

  if (!saw_start_code) {
    const uint8_t nal_type =
        static_cast<uint8_t>(bitstream[0]) & static_cast<uint8_t>(0x1f);
    return nal_type == 5;
  }
  return false;
}

uint32_t rotate_left(uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1_digest(const std::string &input) {
  std::vector<uint8_t> msg(input.begin(), input.end());
  const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8ULL;

  msg.push_back(0x80);
  while ((msg.size() % 64) != 56) {
    msg.push_back(0x00);
  }
  for (int i = 7; i >= 0; --i) {
    msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));
  }

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xEFCDAB89;
  uint32_t h2 = 0x98BADCFE;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xC3D2E1F0;

  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    uint32_t w[80] = {0};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(msg[chunk + (i * 4)]) << 24) |
             (static_cast<uint32_t>(msg[chunk + (i * 4) + 1]) << 16) |
             (static_cast<uint32_t>(msg[chunk + (i * 4) + 2]) << 8) |
             static_cast<uint32_t>(msg[chunk + (i * 4) + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h0;
    uint32_t b = h1;
    uint32_t c = h2;
    uint32_t d = h3;
    uint32_t e = h4;

    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0;
      uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }

      const uint32_t temp =
          rotate_left(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotate_left(b, 30);
      b = a;
      a = temp;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::array<uint8_t, 20> digest{};
  auto write_be = [&digest](size_t offset, uint32_t value) {
    digest[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xff);
    digest[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xff);
    digest[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xff);
    digest[offset + 3] = static_cast<uint8_t>(value & 0xff);
  };
  write_be(0, h0);
  write_be(4, h1);
  write_be(8, h2);
  write_be(12, h3);
  write_be(16, h4);
  return digest;
}

std::string base64_encode(const uint8_t *data, size_t len) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  size_t i = 0;
  while (i < len) {
    const uint32_t octet_a = i < len ? data[i++] : 0;
    const uint32_t octet_b = i < len ? data[i++] : 0;
    const uint32_t octet_c = i < len ? data[i++] : 0;
    const uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

    out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
    out.push_back(i > len + 1 ? '=' : kAlphabet[(triple >> 6) & 0x3f]);
    out.push_back(i > len ? '=' : kAlphabet[triple & 0x3f]);
  }

  const size_t mod = len % 3;
  if (mod > 0) {
    out[out.size() - 1] = '=';
    if (mod == 1) {
      out[out.size() - 2] = '=';
    }
  }
  return out;
}

std::string websocket_accept(const std::string &sec_key) {
  static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const auto digest = sha1_digest(sec_key + kGuid);
  return base64_encode(digest.data(), digest.size());
}

bool send_ws_binary_frame(int fd, const uint8_t *payload, size_t payload_len) {
  std::array<uint8_t, 10> header{};
  size_t header_size = 0;

  header[header_size++] = 0x82; // FIN + binary frame.
  if (payload_len <= 125) {
    header[header_size++] = static_cast<uint8_t>(payload_len);
  } else if (payload_len <= std::numeric_limits<uint16_t>::max()) {
    header[header_size++] = 126;
    header[header_size++] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    header[header_size++] = static_cast<uint8_t>(payload_len & 0xff);
  } else {
    header[header_size++] = 127;
    for (int i = 7; i >= 0; --i) {
      header[header_size++] =
          static_cast<uint8_t>((payload_len >> (8 * i)) & 0xff);
    }
  }

  if (!send_all(fd, header.data(), header_size)) {
    return false;
  }
  if (payload_len == 0) {
    return true;
  }
  return send_all(fd, payload, payload_len);
}

bool send_ws_binary_frame(int fd, const std::string &payload) {
  return send_ws_binary_frame(fd,
                              reinterpret_cast<const uint8_t *>(payload.data()),
                              payload.size());
}

bool stream_mjpeg(int fd, const CaptureParams &params,
                  const std::shared_ptr<Session> &session) {
  const int frame_interval_ms = std::max(1, 1000 / std::max(1, params.fps));
  std::string frame;
  while (true) {
    if (!session->capture || !session->capture->running()) {
      std::this_thread::sleep_for(20ms);
      continue;
    }
    if (session->capture->pixel_format() != PixelFormat::MJPEG ||
        !session->capture->latest_frame(frame)) {
      std::this_thread::sleep_for(10ms);
      continue;
    }
    if (!send_ws_binary_frame(fd, frame)) {
      return false;
    }
    stream::note_frame_sent(*session, frame.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
  }
}

bool stream_h264(int fd, const CaptureParams &params,
                 const std::shared_ptr<Session> &session) {
  const int width = params.width;
  const int height = params.height;
  const int y_size = width * height;
  const int uv_size = (width / 2) * (height / 2);
  const int frame_interval_ms = std::max(1, 1000 / std::max(1, params.fps));

  std::string frame;
  std::string yuv(y_size + (2 * uv_size), '\0');
  uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
  uint8_t *u = y + y_size;
  uint8_t *v = u + uv_size;

  bool first = true;
  uint32_t last_idr = session->idr_request_seq.load();
#ifdef HAS_OPENH264
  H264Encoder encoder;
  bool encoder_ready = false;
#endif

  while (true) {
    if (!session->capture || !session->capture->running()) {
      std::this_thread::sleep_for(20ms);
      continue;
    }
    if (!session->capture->latest_frame(frame)) {
      std::this_thread::sleep_for(10ms);
      continue;
    }

    PixelFormat fmt = session->capture->pixel_format();
    const bool passthrough = fmt == PixelFormat::H264;
    std::string payload;

    if (passthrough) {
      payload = frame;
    } else {
#ifdef HAS_OPENH264
      if (fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) {
        std::this_thread::sleep_for(10ms);
        continue;
      }
      if (!encoder_ready) {
        if (!encoder.init(params)) {
          return false;
        }
        encoder.force_idr();
        encoder_ready = true;
      }
      if (fmt == PixelFormat::YUYV) {
        yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()), width,
                     height, y, u, v);
      } else {
        const uint8_t *src_y = reinterpret_cast<const uint8_t *>(frame.data());
        const uint8_t *src_uv = src_y + (width * height);
        nv12_to_i420(src_y, src_uv, width, height, width, width, y, u, v);
      }
      if (first) {
        encoder.force_idr();
        first = false;
      }
      const uint32_t current_idr = session->idr_request_seq.load();
      if (current_idr > last_idr) {
        encoder.force_idr();
        last_idr = current_idr;
      }
      std::string nal;
      if (!encoder.encode_i420(y, u, v, nal)) {
        std::this_thread::sleep_for(5ms);
        continue;
      }
      if (nal.empty()) {
        continue;
      }
      static const char start_code[] = {0x00, 0x00, 0x00, 0x01};
      payload.reserve(sizeof(start_code) + nal.size());
      payload.append(start_code, sizeof(start_code));
      payload.append(nal);
#else
      return false;
#endif
    }

    if (payload.empty()) {
      continue;
    }
    const bool keyframe = contains_idr(payload);
    if (!send_ws_binary_frame(fd, payload)) {
      return false;
    }
    stream::note_frame_sent(*session, payload.size(), keyframe);
    std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
  }
}

bool stream_raw(int fd, const CaptureParams &params,
                const std::shared_ptr<Session> &session) {
  const int width = params.width;
  const int height = params.height;
  const int y_size = width * height;
  const int uv_size = (width / 2) * (height / 2);
  const int frame_interval_ms = std::max(1, 1000 / std::max(1, params.fps));

  std::string frame;
  std::string i420(y_size + (2 * uv_size), '\0');
  std::string payload(stream::raw_frame_size_bytes(params), '\0');
  uint8_t *y = reinterpret_cast<uint8_t *>(i420.data());
  uint8_t *u = y + y_size;
  uint8_t *v = u + uv_size;

  while (true) {
    if (!session->capture || !session->capture->running()) {
      std::this_thread::sleep_for(20ms);
      continue;
    }
    if (!session->capture->latest_frame(frame)) {
      std::this_thread::sleep_for(10ms);
      continue;
    }

    const PixelFormat fmt = session->capture->pixel_format();
    if (fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) {
      std::this_thread::sleep_for(10ms);
      continue;
    }

    if (fmt == PixelFormat::YUYV) {
      yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()), width,
                   height, y, u, v);
    } else {
      const uint8_t *src_y = reinterpret_cast<const uint8_t *>(frame.data());
      const uint8_t *src_uv = src_y + (width * height);
      nv12_to_i420(src_y, src_uv, width, height, width, width, y, u, v);
    }

    if (params.pixfmt == "rgb24") {
      i420_to_rgb24(y, u, v, width, height,
                    reinterpret_cast<uint8_t *>(payload.data()));
    } else {
      std::memcpy(payload.data(), i420.data(), payload.size());
    }

    if (!send_ws_binary_frame(fd, payload)) {
      return false;
    }
    stream::note_frame_sent(*session, payload.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
  }
}

void set_client_socket_options(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

#endif // defined(__linux__) || defined(__APPLE__)
} // namespace

WebSocketServer::WebSocketServer(SessionManager &sessions,
                                 const CapabilityRegistry &registry,
                                 const AdapterRegistry &adapters,
                                 WsServerConfig config)
    : sessions_(sessions), registry_(registry), adapters_(adapters),
      config_(std::move(config)) {}

WebSocketServer::~WebSocketServer() { stop(); }

bool WebSocketServer::start() {
#if defined(__linux__) || defined(__APPLE__)
  if (running_.load() || config_.port <= 0) {
    return false;
  }

  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    std::cerr << "[ws] failed to create socket" << std::endl;
    return false;
  }

  int reuse = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(config_.port));
  const std::string bind_addr =
      config_.bind_addr.empty() ? "0.0.0.0" : config_.bind_addr;
  if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "[ws] invalid bind address: " << bind_addr << std::endl;
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (bind(listen_fd_, reinterpret_cast<const sockaddr *>(&addr),
           sizeof(addr)) != 0) {
    std::cerr << "[ws] bind failed on " << bind_addr << ":" << config_.port
              << std::endl;
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (listen(listen_fd_, SOMAXCONN) != 0) {
    std::cerr << "[ws] listen failed on port " << config_.port << std::endl;
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  stop_.store(false);
  running_.store(true);
  accept_thread_ = std::thread([this] { accept_loop(); });
  std::cout << "[ws] websocket server listening on " << config_.bind_addr
            << ":" << config_.port << std::endl;
  return true;
#else
  return false;
#endif
}

void WebSocketServer::stop() {
#if defined(__linux__) || defined(__APPLE__)
  if (!running_.load()) {
    return;
  }
  stop_.store(true);
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  running_.store(false);
#endif
}

void WebSocketServer::accept_loop() {
#if defined(__linux__) || defined(__APPLE__)
  while (!stop_.load()) {
    int client_fd = accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (stop_.load()) {
        break;
      }
      std::this_thread::sleep_for(20ms);
      continue;
    }
    set_client_socket_options(client_fd);
    std::thread(&WebSocketServer::handle_client, this, client_fd).detach();
  }
#endif
}

void WebSocketServer::handle_client(int client_fd) {
#if defined(__linux__) || defined(__APPLE__)
  HttpRequest request;
  if (!read_http_request(client_fd, request)) {
    close(client_fd);
    return;
  }

  if (request.method != "GET") {
    send_http_json(client_fd, 405,
                   stream::build_error_json("method_not_allowed",
                                            "only GET is supported"));
    close(client_fd);
    return;
  }

  const auto [path, query] = split_target(request.target);
  if (!is_supported_ws_path(path)) {
    send_http_json(client_fd, 404,
                   stream::build_error_json("not_found", "unknown route"));
    close(client_fd);
    return;
  }

  if (!header_contains_token(request, "connection", "upgrade") ||
      to_lower_copy(get_header(request, "upgrade")) != "websocket") {
    const std::string details =
        "use websocket upgrade with url=" + ws_hint_url(request, config_.port,
                                                        request.target);
    send_http_json(client_fd, 426,
                   stream::build_error_json("upgrade_required", details),
                   {{"Upgrade", "websocket"}});
    close(client_fd);
    return;
  }

  const auto sec_key = get_header(request, "sec-websocket-key");
  if (sec_key.empty()) {
    send_http_json(client_fd, 400,
                   stream::build_error_json("bad_request",
                                            "sec-websocket-key is required"));
    close(client_fd);
    return;
  }

  const auto ws_version = get_header(request, "sec-websocket-version");
  if (!ws_version.empty() && ws_version != "13") {
    send_http_json(client_fd, 400,
                   stream::build_error_json(
                       "bad_request",
                       "sec-websocket-version must be 13 for this endpoint"),
                   {{"Sec-WebSocket-Version", "13"}});
    close(client_fd);
    return;
  }

  std::string error_json;
  auto device_opt = extract_device_id(path, query, error_json);
  if (!device_opt) {
    send_http_json(client_fd, path == "/stream/ws" ? 400 : 404, error_json);
    close(client_fd);
    return;
  }
  std::string device_id = *device_opt;

  CaptureParams params;
  if (!parse_capture_params(query, params, error_json)) {
    send_http_json(client_fd, 400, error_json);
    close(client_fd);
    return;
  }
  const bool is_rtsp = device_id.rfind("rtsp://", 0) == 0;
  if (query.find("media") == query.end()) {
    params.media = "video";
  }
  if (query.find("codec") == query.end()) {
    params.codec = is_rtsp ? "h264" : config_.default_codec;
  }
  if (query.find("container") == query.end()) {
    params.container = "raw";
  }
  if (!query.count("latency") && is_rtsp) {
    params.latency = "ultra";
  }
  if (params.codec == "raw") {
    if (params.pixfmt.empty()) {
      params.pixfmt = "i420";
    }
  } else {
    params.pixfmt.clear();
  }
  if (is_rtsp && params.codec == "raw") {
    send_http_json(client_fd, 409,
                   stream::build_error_json(
                       "incompatible_params",
                       "rtsp sources do not expose raw frames"));
    close(client_fd);
    return;
  }

  const auto media_vr = adapters_.validate_request(params.media, "ws");
  if (!media_vr.ok) {
    send_http_json(client_fd, media_vr.status, media_vr.body);
    close(client_fd);
    return;
  }

  const auto codec_vr = registry_.validate(params, TransportKind::WebSocket);
  if (!codec_vr.ok) {
    send_http_json(client_fd, codec_vr.status, codec_vr.body);
    close(client_fd);
    return;
  }

  auto session = sessions_.get_or_create(device_id, params);
  session->client_count.fetch_add(1);
  session->last_accessed = std::chrono::steady_clock::now();
  session->state.store(SessionState::Live);
  session->teardown_reason.store(TeardownReason::None);

  bool attached = true;
  auto detach_client = [&]() {
    if (!attached) {
      return;
    }
    const int prev = session->client_count.fetch_sub(1);
    if (prev <= 1) {
      session->client_count.store(0);
    }
    sessions_.release_if_idle(device_id);
    attached = false;
  };

  if (!session->capture->running()) {
    session->state.store(SessionState::Warming);
    const auto warming_started_at = std::chrono::steady_clock::now();
    if (!session->capture->start(device_id, session->params)) {
      send_http_json(client_fd, 503,
                     stream::build_error_json("device_unavailable",
                                              "failed to open camera"));
      session->state.store(SessionState::Idle);
      session->teardown_reason.store(TeardownReason::OpenFailed);
      detach_client();
      close(client_fd);
      return;
    }
    stream::sync_session_params(*session);
    session->started = std::chrono::steady_clock::now();
    session->frames_sent.store(0);
    session->bytes_sent.store(0);
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

  if (session->params.codec == "h264") {
    session->idr_request_seq.fetch_add(1);
  }

  std::ostringstream hs;
  hs << "HTTP/1.1 101 Switching Protocols\r\n";
  hs << "Upgrade: websocket\r\n";
  hs << "Connection: Upgrade\r\n";
  hs << "Sec-WebSocket-Accept: " << websocket_accept(sec_key) << "\r\n";
  hs << "X-SilkCast-Width: " << session->params.width << "\r\n";
  hs << "X-SilkCast-Height: " << session->params.height << "\r\n";
  hs << "X-SilkCast-Fps: " << session->params.fps << "\r\n";
  if (session->params.codec == "raw") {
    hs << "X-SilkCast-Pixel-Format: " << session->params.pixfmt << "\r\n";
    hs << "X-SilkCast-Frame-Bytes: "
       << stream::raw_frame_size_bytes(session->params) << "\r\n";
  }
  hs << "X-SilkCast-Codec: " << session->params.codec << "\r\n\r\n";
  const std::string hs_resp = hs.str();
  if (!send_all(client_fd, hs_resp.data(), hs_resp.size())) {
    detach_client();
    close(client_fd);
    return;
  }

  if (session->params.codec == "mjpeg") {
    (void)stream_mjpeg(client_fd, session->params, session);
  } else if (session->params.codec == "raw") {
    (void)stream_raw(client_fd, session->params, session);
  } else if (session->params.codec == "h264") {
    (void)stream_h264(client_fd, session->params, session);
  } else {
    session->teardown_reason.store(TeardownReason::RuntimeError);
  }

  detach_client();
  close(client_fd);
#else
  (void)client_fd;
#endif
}
