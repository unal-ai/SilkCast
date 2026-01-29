#pragma once
#include <cstdint>
#include <cstring>

// Convert a single YUYV 4:2:2 frame to planar I420 (YUV420p).
// Assumes width and height are even.
inline void yuyv_to_i420(const uint8_t* src, int width, int height,
                         uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v) {
  const int uv_width = width / 2;
  for (int y = 0; y < height; y += 2) {
    const uint8_t* row1 = src + y * width * 2;
    const uint8_t* row2 = src + (y + 1) * width * 2;
    uint8_t* yplane1 = dst_y + y * width;
    uint8_t* yplane2 = dst_y + (y + 1) * width;
    uint8_t* urow = dst_u + (y / 2) * uv_width;
    uint8_t* vrow = dst_v + (y / 2) * uv_width;

    for (int x = 0; x < width; x += 2) {
      // Row 1
      uint8_t y0 = row1[2 * x + 0];
      uint8_t u0 = row1[2 * x + 1];
      uint8_t y1 = row1[2 * x + 2];
      uint8_t v0 = row1[2 * x + 3];

      // Row 2
      uint8_t y2 = row2[2 * x + 0];
      uint8_t u1 = row2[2 * x + 1];
      uint8_t y3 = row2[2 * x + 2];
      uint8_t v1 = row2[2 * x + 3];

      yplane1[x] = y0;
      yplane1[x + 1] = y1;
      yplane2[x] = y2;
      yplane2[x + 1] = y3;

      // Average chroma samples for the 2x2 block.
      urow[x / 2] = static_cast<uint8_t>((u0 + u1) / 2);
      vrow[x / 2] = static_cast<uint8_t>((v0 + v1) / 2);
    }
  }
}

// Convert NV12 (Y + interleaved UV) to planar I420 (YUV420p).
// Assumes width and height are even.
inline void nv12_to_i420(const uint8_t* src_y, const uint8_t* src_uv,
                         int width, int height, int src_y_stride,
                         int src_uv_stride, uint8_t* dst_y, uint8_t* dst_u,
                         uint8_t* dst_v) {
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = src_y + y * src_y_stride;
    uint8_t* out = dst_y + y * width;
    std::memcpy(out, row, width);
  }
  const int uv_height = height / 2;
  for (int y = 0; y < uv_height; ++y) {
    const uint8_t* row = src_uv + y * src_uv_stride;
    uint8_t* out_u = dst_u + y * (width / 2);
    uint8_t* out_v = dst_v + y * (width / 2);
    for (int x = 0; x < width; x += 2) {
      out_u[x / 2] = row[x + 0];
      out_v[x / 2] = row[x + 1];
    }
  }
}
