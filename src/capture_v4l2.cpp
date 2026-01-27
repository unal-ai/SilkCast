#ifdef __linux__
#include "capture_v4l2.hpp"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

namespace {
bool xioctl(int fd, unsigned long request, void *arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r != -1;
}
} // namespace

CaptureV4L2::~CaptureV4L2() { stop(); }

bool CaptureV4L2::configure_device(int fd, const CaptureParams &params) {
  v4l2_capability cap{};
  if (!xioctl(fd, VIDIOC_QUERYCAP, &cap)) return false;
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return false;

  // Choose pixel format based on desired codec.
  __u32 pixfmt = params.codec == "h264" ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
  pixel_format_ = params.codec == "h264" ? PixelFormat::YUYV : PixelFormat::MJPEG;

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = params.width;
  fmt.fmt.pix.height = params.height;
  fmt.fmt.pix.pixelformat = pixfmt;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if (!xioctl(fd, VIDIOC_S_FMT, &fmt)) {
    std::cerr << "VIDIOC_S_FMT failed; errno=" << errno << "\n";
    return false;
  }

  // Set FPS if possible.
  v4l2_streamparm sp{};
  sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  sp.parm.capture.timeperframe.numerator = 1;
  sp.parm.capture.timeperframe.denominator = params.fps;
  xioctl(fd, VIDIOC_S_PARM, &sp); // best effort

  return true;
}

bool CaptureV4L2::start(const std::string &device_id, const CaptureParams &params) {
  if (running_) return true;
  device_id_ = device_id;
  params_ = params;

  std::string dev_path = device_id_.rfind("/dev/", 0) == 0 ? device_id_ : "/dev/" + device_id_;
  fd_ = ::open(dev_path.c_str(), O_RDWR);
  if (fd_ < 0) {
    std::cerr << "Failed to open " << dev_path << " errno=" << errno << "\n";
    return false;
  }
  if (!configure_device(fd_, params_)) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  stop_flag_ = false;
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  return true;
}

void CaptureV4L2::stop() {
  stop_flag_ = true;
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  running_ = false;
}

bool CaptureV4L2::latest_frame(std::string &out) {
  std::lock_guard<std::mutex> lock(buf_mu_);
  if (buffer_.empty()) return false;
  out = buffer_;
  return true;
}

void CaptureV4L2::loop() {
  constexpr size_t kMaxFrame = 8 * 1024 * 1024; // up to 1080p YUYV
  std::string local;
  local.resize(kMaxFrame);

  while (!stop_flag_) {
    ssize_t n = ::read(fd_, local.data(), local.size());
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        std::this_thread::sleep_for(5ms);
        continue;
      }
      std::cerr << "read error errno=" << errno << "\n";
      break;
    } else if (n == 0) {
      std::this_thread::sleep_for(5ms);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(buf_mu_);
      buffer_.assign(local.data(), n);
    }
  }
  running_ = false;
}

#endif // __linux__
