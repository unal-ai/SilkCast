#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal fragmented MP4 builder for H.264 (AVC1) Baseline.
class Mp4Fragmenter {
public:
  Mp4Fragmenter(int width, int height, int fps, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);

  // Build init segment (ftyp+moov) once per session.
  std::string build_init_segment() const;

  // Build one fragment (moof+mdat) for a single sample.
  // pts and duration are in timescale units (default timescale 90000).
  std::string build_fragment(const std::vector<uint8_t>& avcc_sample, uint32_t sequence_number,
                             uint64_t base_decode_time, uint32_t sample_duration,
                             bool keyframe) const;

  uint32_t timescale() const { return timescale_; }

private:
  int width_;
  int height_;
  int fps_;
  uint32_t timescale_;
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
};

