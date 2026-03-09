#include "encoder_jpeg.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef HAS_JPEG
extern "C" {
#include <jpeglib.h>
}
#include <csetjmp>
#endif

#include "yuv_convert.hpp"

namespace {

#ifdef HAS_JPEG
struct JpegErrorManager {
  jpeg_error_mgr pub;
  jmp_buf jump_buffer;
};

void jpeg_error_exit(j_common_ptr cinfo) {
  auto *err = reinterpret_cast<JpegErrorManager *>(cinfo->err);
  longjmp(err->jump_buffer, 1);
}

bool encode_rgb24_to_jpeg(const uint8_t *rgb, int width, int height,
                          int quality, std::string &jpeg_out) {
  jpeg_compress_struct cinfo{};
  JpegErrorManager jerr{};
  unsigned char *buffer = nullptr;
  unsigned long buffer_size = 0;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;

  if (setjmp(jerr.jump_buffer) != 0) {
    jpeg_destroy_compress(&cinfo);
    if (buffer != nullptr) {
      std::free(buffer);
    }
    return false;
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buffer, &buffer_size);

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  const int row_stride = width * 3;
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row =
        const_cast<JSAMPROW>(rgb + (cinfo.next_scanline * row_stride));
    (void)jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_out.assign(reinterpret_cast<const char *>(buffer),
                  static_cast<size_t>(buffer_size));
  jpeg_destroy_compress(&cinfo);
  std::free(buffer);
  return true;
}
#endif

} // namespace

bool JpegFrameEncoder::encode(PixelFormat src_fmt, const std::string &src_frame,
                              int width, int height, int quality,
                              std::string &jpeg_out) {
  if (src_fmt == PixelFormat::MJPEG) {
    jpeg_out = src_frame;
    return true;
  }

#ifndef HAS_JPEG
  (void)src_frame;
  (void)width;
  (void)height;
  (void)quality;
  jpeg_out.clear();
  return false;
#else
  if (src_fmt != PixelFormat::YUYV && src_fmt != PixelFormat::NV12) {
    jpeg_out.clear();
    return false;
  }

  const int y_size = width * height;
  const int uv_size = (width / 2) * (height / 2);
  i420_.resize(static_cast<size_t>(y_size + (2 * uv_size)));
  rgb24_.resize(static_cast<size_t>(width * height * 3));

  auto *y = reinterpret_cast<uint8_t *>(i420_.data());
  auto *u = y + y_size;
  auto *v = u + uv_size;

  if (src_fmt == PixelFormat::YUYV) {
    yuyv_to_i420(reinterpret_cast<const uint8_t *>(src_frame.data()), width,
                 height, y, u, v);
  } else {
    const auto *src_y = reinterpret_cast<const uint8_t *>(src_frame.data());
    const auto *src_uv = src_y + (width * height);
    nv12_to_i420(src_y, src_uv, width, height, width, width, y, u, v);
  }

  i420_to_rgb24(y, u, v, width, height,
                reinterpret_cast<uint8_t *>(rgb24_.data()));
  return encode_rgb24_to_jpeg(
      reinterpret_cast<const uint8_t *>(rgb24_.data()), width, height, quality,
      jpeg_out);
#endif
}

bool jpeg_encoder_available() {
#ifdef HAS_JPEG
  return true;
#else
  return false;
#endif
}
