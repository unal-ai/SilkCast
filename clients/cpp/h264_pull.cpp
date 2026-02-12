#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "httplib.h"
#include "wels/codec_api.h"

namespace {
std::string url_encode_path_segment(const std::string &in) {
  static const char *kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
      continue;
    }
    out.push_back('%');
    out.push_back(kHex[(c >> 4) & 0x0F]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

class AnnexBParser {
public:
  template <typename OnNal>
  void append(const uint8_t *data, size_t len, OnNal on_nal) {
    buf_.insert(buf_.end(), data, data + len);

    size_t start = find_start_code(0);
    if (start == std::string::npos) {
      trim_prefix_without_start_code();
      return;
    }
    if (start > 0) {
      buf_.erase(buf_.begin(),
                 buf_.begin() + static_cast<std::ptrdiff_t>(start));
    }

    while (true) {
      if (buf_.size() < 4) return;
      const size_t sc_size = start_code_size_at(0);
      if (sc_size == 0) return;
      const size_t next = find_start_code(sc_size);
      if (next == std::string::npos) {
        return;
      }
      if (next > sc_size) {
        on_nal(buf_.data() + sc_size, next - sc_size);
      }
      buf_.erase(buf_.begin(),
                 buf_.begin() + static_cast<std::ptrdiff_t>(next));
    }
  }

private:
  size_t start_code_size_at(size_t pos) const {
    if (pos + 3 >= buf_.size()) return 0;
    if (buf_[pos] == 0 && buf_[pos + 1] == 0 && buf_[pos + 2] == 1) return 3;
    if (pos + 4 < buf_.size() && buf_[pos] == 0 && buf_[pos + 1] == 0 &&
        buf_[pos + 2] == 0 && buf_[pos + 3] == 1)
      return 4;
    return 0;
  }

  size_t find_start_code(size_t from) const {
    if (buf_.size() < 4 || from >= buf_.size()) return std::string::npos;
    for (size_t i = from; i + 3 < buf_.size(); ++i) {
      if (start_code_size_at(i) != 0) return i;
    }
    return std::string::npos;
  }

  void trim_prefix_without_start_code() {
    if (buf_.size() <= 4) return;
    // Keep a small tail so fragmented start codes can still be matched.
    buf_.erase(buf_.begin(), buf_.end() - 4);
  }

  std::vector<uint8_t> buf_;
};

void print_usage(const char *argv0) {
  std::cerr << "Usage: " << argv0 << " <host> <port> <device>\n"
            << "Example: " << argv0 << " 127.0.0.1 8080 video0\n";
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string host = argv[1];
  const int port = std::atoi(argv[2]);
  const std::string device = argv[3];

  if (port <= 0 || port > 65535) {
    std::cerr << "invalid port: " << argv[2] << "\n";
    return 1;
  }

  ISVCDecoder *decoder = nullptr;
  if (WelsCreateDecoder(&decoder) != 0 || decoder == nullptr) {
    std::cerr << "failed to create OpenH264 decoder\n";
    return 1;
  }

  SDecodingParam dec_param{};
  dec_param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  if (decoder->Initialize(&dec_param) != 0) {
    std::cerr << "failed to initialize OpenH264 decoder\n";
    WelsDestroyDecoder(decoder);
    return 1;
  }

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(5, 0);

  const std::string path = "/stream/live/" + url_encode_path_segment(device) +
                           "?codec=h264&container=raw&w=1280&h=720&fps=30";

  std::cout << "Pulling " << host << ":" << port << path << "\n";

  AnnexBParser parser;
  uint64_t frames = 0;
  auto started_at = std::chrono::steady_clock::now();

  auto result = cli.Get(path, [&](const char *chunk, size_t len) {
    parser.append(reinterpret_cast<const uint8_t *>(chunk), len,
                  [&](const uint8_t *nal, size_t nal_len) {
                    if (nal_len == 0) return;

                    unsigned char *dst[3] = {nullptr, nullptr, nullptr};
                    SBufferInfo info{};
                    DECODING_STATE st = decoder->DecodeFrameNoDelay(
                        nal, static_cast<int>(nal_len), dst, &info);
                    if (st != dsErrorFree) return;
                    if (info.iBufferStatus != 1) return;

                    ++frames;
                    if (frames % 30 == 0) {
                      const auto now = std::chrono::steady_clock::now();
                      const double seconds =
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - started_at)
                              .count() /
                          1000.0;
                      const double fps = seconds > 0.0 ? (frames / seconds) : 0.0;
                      const int width = info.UsrData.sSystemBuffer.iWidth;
                      const int height = info.UsrData.sSystemBuffer.iHeight;
                      std::cout << "decoded_frames=" << frames
                                << " fps=" << fps
                                << " resolution=" << width << "x" << height
                                << "\n";
                    }
                  });
    return true;
  });

  decoder->Uninitialize();
  WelsDestroyDecoder(decoder);

  if (!result) {
    std::cerr << "request failed: " << httplib::to_string(result.error())
              << "\n";
    return 2;
  }
  if (result->status != 200) {
    std::cerr << "server returned status " << result->status << "\n";
    if (!result->body.empty()) std::cerr << result->body << "\n";
    return 3;
  }

  return 0;
}
