#pragma once

#include <string>
#include <vector>

#include "types.hpp"

class CaptureInterface {
public:
  virtual ~CaptureInterface() = default;

  virtual bool start(const std::string &device_id,
                     const CaptureParams &params) = 0;
  virtual void stop() = 0;
  virtual bool latest_frame(std::string &out) = 0;
  virtual bool running() const = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual int fps() const = 0;
  virtual PixelFormat pixel_format() const = 0;

  virtual void get_sps_pps(std::vector<uint8_t> &sps,
                           std::vector<uint8_t> &pps) {
    sps.clear();
    pps.clear();
  }
};
