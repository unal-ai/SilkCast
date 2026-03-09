#pragma once

#include <string>

#include "types.hpp"

class JpegFrameEncoder {
public:
  bool encode(PixelFormat src_fmt, const std::string &src_frame, int width,
              int height, int quality, std::string &jpeg_out);

private:
  std::string i420_;
  std::string rgb24_;
};

bool jpeg_encoder_available();
