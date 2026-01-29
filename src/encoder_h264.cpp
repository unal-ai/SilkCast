#include "encoder_h264.hpp"

#include <cstring>

#include "types.hpp"

#ifdef HAS_OPENH264

#include "wels/codec_api.h"

H264Encoder::~H264Encoder() {
  if (enc_) {
    enc_->Uninitialize();
    WelsDestroySVCEncoder(enc_);
    enc_ = nullptr;
  }
}

bool H264Encoder::init(const CaptureParams &params) {
  width_ = params.width;
  height_ = params.height;
  fps_ = params.fps;
  bitrate_kbps_ = params.bitrate_kbps;

  if (WelsCreateSVCEncoder(&enc_) != 0 || enc_ == nullptr) {
    return false;
  }

  SEncParamBase p{};
  p.iUsageType = CAMERA_VIDEO_REAL_TIME;
  p.iPicWidth = width_;
  p.iPicHeight = height_;
  p.iTargetBitrate = bitrate_kbps_ * 1000;
  p.iRCMode = RC_BITRATE_MODE;
  p.fMaxFrameRate = static_cast<float>(fps_);

  if (enc_->Initialize(&p) != 0) {
    return false;
  }

  // Note: ENCODER_OPTION_PROFILE removed - API changed in OpenH264 2.6.0, use
  // default Set IDR interval (keyframe interval).
  int gop = params.gop > 0 ? params.gop : 30;
  enc_->SetOption(ENCODER_OPTION_IDR_INTERVAL, &gop);
  // Disable frame skipping for consistent streaming.
  bool frame_skip = false;
  enc_->SetOption(ENCODER_OPTION_RC_FRAME_SKIP, &frame_skip);

  return true;
}

bool H264Encoder::encode_i420(const uint8_t *y, const uint8_t *u,
                              const uint8_t *v, std::string &out) {
  if (!enc_)
    return false;

  const int y_size = width_ * height_;
  const int uv_width = width_ / 2;
  const int uv_height = height_ / 2;
  const int uv_size = uv_width * uv_height;

  SSourcePicture pic{};
  pic.iPicWidth = width_;
  pic.iPicHeight = height_;
  pic.iColorFormat = videoFormatI420;
  pic.iStride[0] = width_;
  pic.iStride[1] = uv_width;
  pic.iStride[2] = uv_width;
  pic.pData[0] = const_cast<unsigned char *>(y);
  pic.pData[1] = const_cast<unsigned char *>(u);
  pic.pData[2] = const_cast<unsigned char *>(v);

  SFrameBSInfo info{};
  if (enc_->EncodeFrame(&pic, &info) != 0) {
    return false;
  }

  out.clear();
  for (int i = 0; i < info.iLayerNum; ++i) {
    const SLayerBSInfo &layer = info.sLayerInfo[i];
    for (int j = 0; j < layer.iNalCount; ++j) {
      unsigned char *p = layer.pBsBuf + layer.pNalLengthInByte[j];
      (void)p; // silence unused warning
    }
    int total = 0;
    for (int j = 0; j < layer.iNalCount; ++j)
      total += layer.pNalLengthInByte[j];
    out.append(reinterpret_cast<char *>(layer.pBsBuf), total);
  }
  return !out.empty();
}

void H264Encoder::force_idr() {
  if (!enc_)
    return;
  enc_->ForceIntraFrame(true);
}

#else // HAS_OPENH264

H264Encoder::~H264Encoder() = default;
bool H264Encoder::init(const CaptureParams &) { return false; }
bool H264Encoder::encode_i420(const uint8_t *, const uint8_t *, const uint8_t *,
                              std::string &) {
  return false;
}
void H264Encoder::force_idr() {}

#endif
