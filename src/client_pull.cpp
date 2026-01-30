#include "client_pull.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"

#ifdef HAS_OPENH264
#include "wels/codec_api.h"
#endif

// Minimal Annex-B splitter
class AnnexBParser {
public:
  void feed(const char *data, size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
  }

  std::optional<std::vector<uint8_t>> next_nal() {
    size_t start = find_start_code(0);
    if (start == std::string::npos) {
      // Keep a small tail to avoid splitting a start code
      if (buffer_.size() > 3) {
        buffer_.erase(buffer_.begin(), buffer_.end() - 3);
      }
      return std::nullopt;
    }
    size_t next = find_start_code(start + 3);
    if (next == std::string::npos) {
      if (start > 0)
        buffer_.erase(buffer_.begin(), buffer_.begin() + start);
      return std::nullopt;
    }
    const size_t prefix_len =
        (buffer_[start + 2] == 0x01) ? 3 : 4; // 00 00 01 or 00 00 00 01
    size_t data_begin = start + prefix_len;
    std::vector<uint8_t> nal(buffer_.begin() + data_begin,
                             buffer_.begin() + next);
    buffer_.erase(buffer_.begin(), buffer_.begin() + next);
    return nal;
  }

private:
  size_t find_start_code(size_t from) {
    for (size_t k = from; k + 3 < buffer_.size(); ++k) {
      if (buffer_[k] == 0x00 && buffer_[k + 1] == 0x00) {
        if (buffer_[k + 2] == 0x01)
          return k;
        if (k + 4 <= buffer_.size() && buffer_[k + 2] == 0x00 &&
            buffer_[k + 3] == 0x01)
          return k;
      }
    }
    return std::string::npos;
  }

  std::vector<uint8_t> buffer_;
};

struct DecodedFrame {
  int width = 0;
  int height = 0;
  const uint8_t *y = nullptr;
  const uint8_t *u = nullptr;
  const uint8_t *v = nullptr;
};

#ifdef HAS_OPENH264
class OpenH264Decoder {
public:
  OpenH264Decoder() {
    if (WelsCreateDecoder(&decoder_) != 0) {
      throw std::runtime_error("WelsCreateDecoder failed");
    }
    SDecodingParam param{};
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.uiTargetDqLayer = UCHAR_MAX;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    param.sVideoProperty.size = sizeof(param.sVideoProperty);
    if (decoder_->Initialize(&param) != 0) {
      throw std::runtime_error("OpenH264 decoder init failed");
    }
  }

  ~OpenH264Decoder() { WelsDestroyDecoder(decoder_); }

  std::optional<DecodedFrame> decode(const std::vector<uint8_t> &nal) {
    if (nal.empty())
      return std::nullopt;
    SBufferInfo info{};
    uint8_t *dst[3] = {nullptr, nullptr, nullptr};
    DECODING_STATE st = decoder_->DecodeFrameNoDelay(
        nal.data(), static_cast<int>(nal.size()), dst, &info);
    if (st != dsErrorFree && st != dsFramePending)
      return std::nullopt;
    if (info.iBufferStatus != 1)
      return std::nullopt;
    DecodedFrame f;
    f.width = info.UsrData.sSystemBuffer.iWidth;
    f.height = info.UsrData.sSystemBuffer.iHeight;
    f.y = dst[0];
    f.u = dst[1];
    f.v = dst[2];
    return f;
  }

private:
  ISVCDecoder *decoder_{nullptr};
};
#endif

int run_client(const std::string &connect_to, const std::string &device_id) {
  std::string host = connect_to;
  int port = 8080;
  size_t colon = connect_to.find(':');
  if (colon != std::string::npos) {
    host = connect_to.substr(0, colon);
    port = std::stoi(connect_to.substr(colon + 1));
  }

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5, 0);
  cli.set_read_timeout(10, 0);

  // Defaults for pull
  int w = 1280;
  int h = 720;
  int fps = 30;

  std::string target =
      "/stream/live/" + device_id + "?codec=h264&w=" + std::to_string(w) +
      "&h=" + std::to_string(h) + "&fps=" + std::to_string(fps);

  AnnexBParser parser;
#ifdef HAS_OPENH264
  OpenH264Decoder decoder;
#else
  std::cerr << "OpenH264 NOT enabled. Cannot decode stream." << std::endl;
  return 1;
#endif

  std::cout << "[Client] Connecting to " << host << ":" << port << target
            << std::endl;

  std::atomic<int> frames{0};
  auto t0 = std::chrono::steady_clock::now();

  auto res =
      cli.Get(target.c_str(), {{"Accept", "video/H264"}},
              [&](const char *data, size_t len) {
                parser.feed(data, len);
                while (auto nal = parser.next_nal()) {
#ifdef HAS_OPENH264
                  auto out = decoder.decode(*nal);
                  if (out) {
                    ++frames;
                    if (frames % 30 == 0) {
                      auto now = std::chrono::steady_clock::now();
                      double sec =
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - t0)
                              .count() /
                          1000.0;
                      double approx_fps = frames / std::max(0.001, sec);
                      std::cout << "\rDecoded " << frames << " frames ("
                                << (out->width) << "x" << (out->height)
                                << ") @ " << approx_fps << " fps   "
                                << std::flush;
                    }
                  }
#endif
                }
                return true; // keep streaming
              });

  if (!res) {
    std::cerr << "\n[Client] Connection failed or stream ended." << std::endl;
    return 1;
  }
  std::cout << "\n[Client] Stream ended (status " << res->status << ")"
            << std::endl;
  return 0;
}
