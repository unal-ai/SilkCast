#ifdef __linux__
#include "capture_v4l2.hpp"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
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

PixelFormat v4l2_to_pixel_format(__u32 fmt) {
  switch (fmt) {
  case V4L2_PIX_FMT_MJPEG:
    return PixelFormat::MJPEG;
  case V4L2_PIX_FMT_YUYV:
    return PixelFormat::YUYV;
  case V4L2_PIX_FMT_NV12:
    return PixelFormat::NV12;
  default:
    return PixelFormat::UNKNOWN;
  }
}

std::string fourcc_to_string(__u32 fmt) {
  char fourcc[5] = {static_cast<char>(fmt & 0xFF),
                    static_cast<char>((fmt >> 8) & 0xFF),
                    static_cast<char>((fmt >> 16) & 0xFF),
                    static_cast<char>((fmt >> 24) & 0xFF), 0};
  return std::string(fourcc);
}
} // namespace

CaptureV4L2::~CaptureV4L2() { stop(); }

void CaptureV4L2::cleanup_mmap_buffers() {
  for (unsigned i = 0; i < num_buffers_; ++i) {
    if (buffers_[i].start && buffers_[i].start != MAP_FAILED) {
      munmap(buffers_[i].start, buffers_[i].length);
    }
  }
  for (unsigned i = 0; i < kNumBuffers; ++i) {
    buffers_[i].start = nullptr;
    buffers_[i].length = 0;
  }
  num_buffers_ = 0;
}

void CaptureV4L2::cleanup_mmap_setup_failure(int fd) {
  if (!use_mmap_) {
    return;
  }
  // Best-effort cleanup for partial mmap setup so retries don't leak buffers.
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  xioctl(fd, VIDIOC_STREAMOFF, &type);
  cleanup_mmap_buffers();

  v4l2_requestbuffers req{};
  req.count = 0;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  xioctl(fd, VIDIOC_REQBUFS, &req);
}

bool CaptureV4L2::configure_device(int fd, CaptureParams &params) {
  cleanup_mmap_buffers();

  v4l2_capability cap{};
  if (!xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
    std::cerr << "VIDIOC_QUERYCAP failed; errno=" << errno << "\n";
    return false;
  }
  // Use device_caps if available (modern V4L2), else fall back to capabilities.
  __u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                         : cap.capabilities;
  std::cerr << "Camera: " << cap.card << ", caps=0x" << std::hex << caps
            << std::dec << "\n";
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
    std::cerr << "No V4L2_CAP_VIDEO_CAPTURE\n";
    return false;
  }
  use_mmap_ = (caps & V4L2_CAP_STREAMING) != 0;
  if (!use_mmap_ && !(caps & V4L2_CAP_READWRITE)) {
    std::cerr << "Camera supports neither streaming nor read/write\n";
    return false;
  }
  std::cerr << "Using " << (use_mmap_ ? "mmap streaming" : "read()") << "\n";

  // Choose pixel format based on desired codec.
  __u32 pixfmt =
      params.codec == "h264" ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
  std::cerr << "Setting format: " << params.width << "x" << params.height
            << " codec=" << params.codec << " pixfmt=0x" << std::hex << pixfmt
            << std::dec << "\n";

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
  params.width = static_cast<int>(fmt.fmt.pix.width);
  params.height = static_cast<int>(fmt.fmt.pix.height);
  pixel_format_ = v4l2_to_pixel_format(fmt.fmt.pix.pixelformat);
  if (pixel_format_ == PixelFormat::UNKNOWN) {
    std::cerr << "Unsupported pixel format negotiated: "
              << fourcc_to_string(fmt.fmt.pix.pixelformat) << "\n";
    return false;
  }
  if (params.codec == "mjpeg" && pixel_format_ != PixelFormat::MJPEG) {
    std::cerr << "Device did not accept MJPEG, got "
              << fourcc_to_string(fmt.fmt.pix.pixelformat) << "\n";
    return false;
  }
  if (params.codec == "h264" && pixel_format_ != PixelFormat::YUYV &&
      pixel_format_ != PixelFormat::NV12) {
    std::cerr << "Device did not provide raw frames for H264, got "
              << fourcc_to_string(fmt.fmt.pix.pixelformat) << "\n";
    return false;
  }
  if (params.codec == "mjpeg") {
    // Clamp to driver-friendly range.
    params.quality = std::clamp(params.quality, 1, 100);
    bool quality_set = false;
    __u32 applied_ctrl = 0;

    v4l2_control ctrl{};
    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = params.quality;
    if (xioctl(fd, VIDIOC_S_CTRL, &ctrl)) {
      quality_set = true;
      applied_ctrl = ctrl.id;
    } else {
      std::cerr << "VIDIOC_S_CTRL(JPEG_COMPRESSION_QUALITY) failed; errno="
                << errno << "\n";
    }
    if (!quality_set) {
      ctrl.id = V4L2_CID_JPEG_Q_FACTOR;
      ctrl.value = params.quality;
      if (xioctl(fd, VIDIOC_S_CTRL, &ctrl)) {
        quality_set = true;
        applied_ctrl = ctrl.id;
      } else {
        std::cerr << "VIDIOC_S_CTRL(JPEG_Q_FACTOR) failed; errno=" << errno
                  << "\n";
      }
    }
    if (quality_set && applied_ctrl != 0) {
      v4l2_control get{};
      get.id = applied_ctrl;
      if (xioctl(fd, VIDIOC_G_CTRL, &get)) {
        params.quality = get.value;
      }
      std::cerr << "MJPEG quality set to " << params.quality
                << " via control 0x" << std::hex << applied_ctrl << std::dec
                << "\n";
    }
  }
  std::cerr << "Format set: " << params.width << "x" << params.height
            << " fourcc=" << fourcc_to_string(fmt.fmt.pix.pixelformat) << "\n";
  frame_size_ = fmt.fmt.pix.sizeimage;

  // Set FPS if possible.
  v4l2_streamparm sp{};
  sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  sp.parm.capture.timeperframe.numerator = 1;
  sp.parm.capture.timeperframe.denominator = params.fps;
  xioctl(fd, VIDIOC_S_PARM, &sp); // best effort
  if (xioctl(fd, VIDIOC_G_PARM, &sp)) {
    const auto num = sp.parm.capture.timeperframe.numerator;
    const auto den = sp.parm.capture.timeperframe.denominator;
    if (num > 0 && den > 0) {
      const int fps = static_cast<int>(den / num);
      if (fps > 0) {
        params.fps = fps;
      }
    }
  }

  if (use_mmap_) {
    // Request buffers
    v4l2_requestbuffers req{};
    req.count = kNumBuffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (!xioctl(fd, VIDIOC_REQBUFS, &req) || req.count < 2) {
      std::cerr << "VIDIOC_REQBUFS failed; errno=" << errno << "\n";
      cleanup_mmap_setup_failure(fd);
      return false;
    }
    std::cerr << "Requested " << req.count << " buffers\n";

    // mmap each buffer
    for (unsigned i = 0; i < req.count && i < kNumBuffers; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (!xioctl(fd, VIDIOC_QUERYBUF, &buf)) {
        std::cerr << "VIDIOC_QUERYBUF failed; errno=" << errno << "\n";
        cleanup_mmap_setup_failure(fd);
        return false;
      }
      buffers_[i].length = buf.length;
      buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, buf.m.offset);
      if (buffers_[i].start == MAP_FAILED) {
        std::cerr << "mmap failed; errno=" << errno << "\n";
        cleanup_mmap_setup_failure(fd);
        return false;
      }
      num_buffers_++;
    }

    // Queue all buffers
    for (unsigned i = 0; i < num_buffers_; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (!xioctl(fd, VIDIOC_QBUF, &buf)) {
        std::cerr << "VIDIOC_QBUF failed; errno=" << errno << "\n";
        cleanup_mmap_setup_failure(fd);
        return false;
      }
    }

    // Start streaming
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!xioctl(fd, VIDIOC_STREAMON, &type)) {
      std::cerr << "VIDIOC_STREAMON failed; errno=" << errno << "\n";
      cleanup_mmap_setup_failure(fd);
      return false;
    }
    std::cerr << "Streaming started\n";
  }

  return true;
}

bool CaptureV4L2::start(const std::string &device_id,
                        const CaptureParams &params) {
  if (running_)
    return true;
  device_id_ = device_id;
  params_ = params;

  std::string dev_path =
      device_id_.rfind("/dev/", 0) == 0 ? device_id_ : "/dev/" + device_id_;
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
  if (thread_.joinable())
    thread_.join();

  if (fd_ >= 0) {
    if (use_mmap_) {
      // Stop streaming
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      xioctl(fd_, VIDIOC_STREAMOFF, &type);

      // Unmap buffers
      cleanup_mmap_buffers();
    }
    ::close(fd_);
    fd_ = -1;
  }
  running_ = false;
}

bool CaptureV4L2::latest_frame(std::string &out) {
  std::lock_guard<std::mutex> lock(buf_mu_);
  if (buffer_.empty())
    return false;
  out = buffer_;
  return true;
}

void CaptureV4L2::loop() {
  if (use_mmap_) {
    loop_mmap();
  } else {
    loop_read();
  }
  running_ = false;
}

void CaptureV4L2::loop_mmap() {
  while (!stop_flag_) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms timeout

    int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      std::cerr << "select error errno=" << errno << "\n";
      break;
    }
    if (r == 0)
      continue; // timeout

    // Dequeue buffer
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (!xioctl(fd_, VIDIOC_DQBUF, &buf)) {
      if (errno == EAGAIN)
        continue;
      std::cerr << "VIDIOC_DQBUF failed; errno=" << errno << "\n";
      break;
    }

    // Copy frame data
    {
      std::lock_guard<std::mutex> lock(buf_mu_);
      buffer_.assign(static_cast<char *>(buffers_[buf.index].start),
                     buf.bytesused);
    }

    // Requeue buffer
    if (!xioctl(fd_, VIDIOC_QBUF, &buf)) {
      std::cerr << "VIDIOC_QBUF (requeue) failed; errno=" << errno << "\n";
      break;
    }
  }
}

void CaptureV4L2::loop_read() {
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
}

#endif // __linux__
