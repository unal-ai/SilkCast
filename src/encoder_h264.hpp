#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct CaptureParams;

#ifdef HAS_OPENH264
#include "wels/codec_api.h"
#endif

class H264Encoder {
public:
  H264Encoder() = default;
  ~H264Encoder();

  bool init(const CaptureParams &params);
  // Encodes an I420 frame (Y plane first, then U, then V).
  bool encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v, std::string &out);
  void force_idr();

private:
#ifdef HAS_OPENH264
  ISVCEncoder *enc_{nullptr};
  int width_{640};
  int height_{480};
  int fps_{15};
  int bitrate_kbps_{256};
#endif
};
