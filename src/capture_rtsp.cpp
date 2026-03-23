#include "capture_rtsp.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {
// Base64 decoding lookup table.
static const int kB64Index[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,
    3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, -1, -1, -1, -1, -1};

std::vector<uint8_t> base64_decode(const std::string &in) {
  std::vector<uint8_t> out;
  int val = 0;
  int valb = -8;
  for (unsigned char c : in) {
    if (c < 128 && kB64Index[c] != -1) {
      val = (val << 6) + kB64Index[c];
      valb += 6;
      if (valb >= 0) {
        out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
        valb -= 8;
      }
    }
  }
  return out;
}
} // namespace

CaptureRTSP::~CaptureRTSP() { stop(); }

bool CaptureRTSP::start(const std::string &url, const CaptureParams &params) {
  if (running_) return true;
  url_ = url;
  params_ = params;
  stop_flag_ = false;
  frame_sequence_.store(0, std::memory_order_relaxed);
  running_ = true;
  thread_ = std::thread([this] { loop(); });

  // Fail fast when connect cannot be established.
  for (int i = 0; i < 10; ++i) {
    if (!running_) return false;
    if (sock_ >= 0) return true;
    std::this_thread::sleep_for(50ms);
  }
  return sock_ >= 0;
}

void CaptureRTSP::stop() {
  stop_flag_ = true;
  if (sock_ >= 0) {
    shutdown(sock_, SHUT_RDWR);
    close(sock_);
    sock_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  running_ = false;
  frame_sequence_.store(0, std::memory_order_relaxed);
}

bool CaptureRTSP::latest_frame(std::string &out) {
  std::lock_guard<std::mutex> lock(buf_mu_);
  if (buffer_.empty()) return false;
  out = buffer_;
  return true;
}

void CaptureRTSP::get_sps_pps(std::vector<uint8_t> &sps,
                              std::vector<uint8_t> &pps) {
  std::lock_guard<std::mutex> lock(meta_mu_);
  sps = sps_;
  pps = pps_;
}

bool CaptureRTSP::parse_url(const std::string &url, std::string &host,
                            int &port, std::string &path) {
  if (url.find("rtsp://") != 0) return false;
  std::string clean = url.substr(7);
  size_t slash = clean.find('/');
  std::string authority =
      (slash == std::string::npos) ? clean : clean.substr(0, slash);
  path = (slash == std::string::npos) ? "/" : clean.substr(slash);

  // Drop auth when present: user:pass@
  size_t at = authority.find('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);

  size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    port = std::stoi(authority.substr(colon + 1));
  } else {
    host = authority;
    port = 554;
  }
  return true;
}

int CaptureRTSP::send_cmd(int sock, const std::string &cmd) {
  return static_cast<int>(send(sock, cmd.c_str(), cmd.size(), 0));
}

bool CaptureRTSP::read_line(int sock, std::string &line) {
  line.clear();
  char c;
  while (true) {
    ssize_t n = recv(sock, &c, 1, 0);
    if (n <= 0) return false;
    if (c == '\r') continue;
    if (c == '\n') break;
    line += c;
  }
  return true;
}

bool CaptureRTSP::read_response(int sock, std::string &response) {
  response.clear();
  std::string line;
  int content_length = 0;
  while (read_line(sock, line)) {
    if (line.empty()) break;
    response += line + "\n";
    if (line.find("Content-Length:") == 0) {
      content_length = std::stoi(line.substr(16));
    }
  }

  if (content_length > 0) {
    std::vector<char> body(content_length);
    int total = 0;
    while (total < content_length) {
      ssize_t n = recv(sock, body.data() + total, content_length - total, 0);
      if (n <= 0) return false;
      total += n;
    }
    response += "\r\n";
    response.append(body.begin(), body.end());
  }
  return true;
}

bool CaptureRTSP::parse_sdp(const std::string &sdp, std::string &control_url) {
  std::istringstream iss(sdp);
  std::string line;
  bool in_video = false;
  std::string video_control;

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find("m=video") == 0) {
      in_video = true;
    } else if (line.find("m=") == 0) {
      in_video = false;
    }

    if (!in_video) continue;

    // Parse out SPS/PPS advertised in SDP.
    size_t pos = line.find("sprop-parameter-sets=");
    if (pos != std::string::npos) {
      std::string params = line.substr(pos + 21);
      size_t comma = params.find(',');
      std::string sps_str = params.substr(0, comma);
      std::string pps_str =
          (comma != std::string::npos) ? params.substr(comma + 1) : "";
      size_t end = pps_str.find_first_of(" ;");
      if (end != std::string::npos) pps_str = pps_str.substr(0, end);

      auto sps = base64_decode(sps_str);
      auto pps = base64_decode(pps_str);
      std::lock_guard<std::mutex> lock(meta_mu_);
      sps_ = std::move(sps);
      pps_ = std::move(pps);
    }

    if (line.find("a=control:") == 0) {
      video_control = line.substr(10);
    }
  }

  if (video_control.empty()) return false;
  if (video_control.find("rtsp://") == 0) {
    control_url = video_control;
  } else {
    control_url = url_ + "/" + video_control;
  }
  return true;
}

bool CaptureRTSP::connect_rtsp() {
  std::string host;
  int port = 0;
  std::string path;
  if (!parse_url(url_, host, port, path)) return false;

  struct hostent *server = gethostbyname(host.c_str());
  if (!server) return false;

  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) return false;

  struct sockaddr_in serv_addr {};
  serv_addr.sin_family = AF_INET;
  std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
  serv_addr.sin_port = htons(port);
  if (connect(sock_, reinterpret_cast<struct sockaddr *>(&serv_addr),
              sizeof(serv_addr)) < 0) {
    close(sock_);
    sock_ = -1;
    return false;
  }

  int cseq = 1;
  std::ostringstream options;
  options << "OPTIONS " << url_ << " RTSP/1.0\r\n"
          << "CSeq: " << cseq++ << "\r\n"
          << "User-Agent: SilkCast\r\n\r\n";
  send_cmd(sock_, options.str());

  std::string response;
  if (!read_response(sock_, response)) return false;

  std::ostringstream describe;
  describe << "DESCRIBE " << url_ << " RTSP/1.0\r\n"
           << "CSeq: " << cseq++ << "\r\n"
           << "Accept: application/sdp\r\n\r\n";
  send_cmd(sock_, describe.str());
  if (!read_response(sock_, response)) return false;

  std::string control_url;
  parse_sdp(response, control_url);
  if (control_url.empty()) control_url = url_;
  control_url_ = control_url;

  std::ostringstream setup;
  setup << "SETUP " << control_url << " RTSP/1.0\r\n"
        << "CSeq: " << cseq++ << "\r\n"
        << "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
  send_cmd(sock_, setup.str());
  if (!read_response(sock_, response)) return false;

  session_id_.clear();
  size_t sess_pos = response.find("Session: ");
  if (sess_pos != std::string::npos) {
    size_t end = response.find_first_of(";\r\n", sess_pos + 9);
    session_id_ = response.substr(sess_pos + 9, end - (sess_pos + 9));
  }

  std::ostringstream play;
  play << "PLAY " << url_ << " RTSP/1.0\r\n"
       << "CSeq: " << cseq++ << "\r\n";
  if (!session_id_.empty()) play << "Session: " << session_id_ << "\r\n";
  play << "Range: npt=0.000-\r\n\r\n";
  send_cmd(sock_, play.str());
  if (!read_response(sock_, response)) return false;

  return true;
}

void CaptureRTSP::send_keepalive() {
  if (sock_ < 0) return;
  static std::atomic<int> cseq{1000};
  int seq = cseq.fetch_add(1);

  std::ostringstream opts;
  opts << "OPTIONS " << url_ << " RTSP/1.0\r\n"
       << "CSeq: " << seq << "\r\n";
  if (!session_id_.empty()) opts << "Session: " << session_id_ << "\r\n";
  opts << "User-Agent: SilkCast\r\n\r\n";
  send_cmd(sock_, opts.str());

  std::string resp;
  read_response(sock_, resp);

  std::ostringstream gp;
  gp << "GET_PARAMETER " << control_url_ << " RTSP/1.0\r\n"
     << "CSeq: " << (seq + 1) << "\r\n";
  if (!session_id_.empty()) gp << "Session: " << session_id_ << "\r\n";
  gp << "User-Agent: SilkCast\r\n\r\n";
  send_cmd(sock_, gp.str());
  read_response(sock_, resp);
}

void CaptureRTSP::loop() {
  int retry_count = 0;
  while (!stop_flag_) {
    if (sock_ < 0) {
      if (!connect_rtsp()) {
        std::this_thread::sleep_for(2s);
        retry_count++;
        if (retry_count > 5) {
          running_ = false;
          break;
        }
        continue;
      }
      retry_count = 0;
      struct timeval tv {};
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv),
                 sizeof(tv));
      last_keepalive_ = std::chrono::steady_clock::now();
    }

    // Interleaved RTP-over-TCP frame header: '$' channel len_hi len_lo
    char header[4];
    ssize_t n = recv(sock_, header, 4, MSG_PEEK);
    if (n <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_keepalive_)
                .count() >= 25) {
          send_keepalive();
          last_keepalive_ = now;
        }
        continue;
      }
      close(sock_);
      sock_ = -1;
      continue;
    }

    if (header[0] != '$') {
      char t;
      recv(sock_, &t, 1, 0);
      continue;
    }

    recv(sock_, header, 4, 0);
    int channel = header[1];
    int len = (static_cast<uint8_t>(header[2]) << 8) |
              static_cast<uint8_t>(header[3]);
    if (len <= 0) continue;

    std::vector<uint8_t> packet(static_cast<size_t>(len));
    int total = 0;
    while (total < len) {
      int r = recv(sock_, packet.data() + total, len - total, 0);
      if (r <= 0) {
        close(sock_);
        sock_ = -1;
        break;
      }
      total += r;
    }
    if (total < len) continue;
    if (channel != 0) continue; // Skip RTCP.

    // RTP: minimum 12-byte header.
    if (len < 12) continue;
    uint8_t *rtp = packet.data();
    int csrc_count = rtp[0] & 0x0F;
    int header_len = 12 + csrc_count * 4;
    if (len < header_len) continue;

    uint8_t *payload = rtp + header_len;
    int payload_len = len - header_len;
    if (payload_len <= 0) continue;

    static const char start_code[] = {0, 0, 0, 1};
    std::string nalu;
    uint8_t type = payload[0] & 0x1F;

    if (type >= 1 && type <= 23) {
      nalu.append(start_code, 4);
      nalu.append(reinterpret_cast<const char *>(payload), payload_len);
    } else if (type == 24) { // STAP-A
      const uint8_t *ptr = payload + 1;
      const uint8_t *end = payload + payload_len;
      while (ptr + 2 < end) {
        uint16_t nlen = (ptr[0] << 8) | ptr[1];
        ptr += 2;
        if (ptr + nlen > end) break;
        nalu.append(start_code, 4);
        nalu.append(reinterpret_cast<const char *>(ptr), nlen);
        ptr += nlen;
      }
    } else if (type == 28) { // FU-A
      if (payload_len < 2) continue;
      uint8_t indicator = payload[0];
      uint8_t fu_header = payload[1];
      bool start = (fu_header & 0x80) != 0;
      bool end = (fu_header & 0x40) != 0;
      uint8_t orig_nal = (indicator & 0xE0) | (fu_header & 0x1F);

      static std::string frag_buf;
      if (start) {
        frag_buf.clear();
        frag_buf.append(start_code, 4);
        char h = static_cast<char>(orig_nal);
        frag_buf.append(&h, 1);
      }
      frag_buf.append(reinterpret_cast<const char *>(payload + 2),
                      payload_len - 2);
      if (end) nalu = frag_buf;
    }

    if (!nalu.empty()) {
      {
        std::lock_guard<std::mutex> lock(buf_mu_);
        buffer_ = nalu;
      }
      frame_sequence_.fetch_add(1, std::memory_order_relaxed);

      // Try extracting SPS/PPS from in-band stream.
      size_t pos = 0;
      while (pos + 5 < nalu.size()) {
        if (nalu[pos] == 0 && nalu[pos + 1] == 0 && nalu[pos + 2] == 0 &&
            nalu[pos + 3] == 1) {
          uint8_t t = static_cast<uint8_t>(nalu[pos + 4]) & 0x1F;
          size_t start = pos + 4;
          size_t next = nalu.find(std::string("\x00\x00\x00\x01", 4), start);
          size_t slen = (next == std::string::npos) ? (nalu.size() - start)
                                                    : (next - start);
          if (t == 7 || t == 8) {
            std::vector<uint8_t> payload_bytes(nalu.begin() + start,
                                               nalu.begin() + start + slen);
            std::lock_guard<std::mutex> lk(meta_mu_);
            if (t == 7) sps_ = std::move(payload_bytes);
            if (t == 8) pps_ = std::move(payload_bytes);
          }
        }
        pos++;
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_keepalive_)
            .count() >= 25) {
      send_keepalive();
      last_keepalive_ = now;
    }
  }
  running_ = false;
}
