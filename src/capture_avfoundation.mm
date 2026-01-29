#ifdef __APPLE__
#include "capture_v4l2.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <AvailabilityMacros.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
bool parse_index(const std::string &id, int &index) {
  size_t pos = std::string::npos;
  if (id.rfind("video", 0) == 0) {
    pos = 5;
  } else if (id.rfind("/dev/video", 0) == 0) {
    pos = 9;
  } else if (id.rfind("camera", 0) == 0) {
    pos = 6;
  }
  if (pos == std::string::npos || pos >= id.size())
    return false;
  char *end = nullptr;
  long value = std::strtol(id.c_str() + pos, &end, 10);
  if (end == id.c_str() + pos)
    return false;
  index = static_cast<int>(value);
  return true;
}

NSArray<AVCaptureDevice *> *enumerate_devices() {
  NSMutableArray<AVCaptureDeviceType> *types =
      [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
  [types addObject:AVCaptureDeviceTypeExternal];
#else
  [types addObject:AVCaptureDeviceTypeExternalUnknown];
#endif
  if (@available(macOS 13.0, *)) {
    [types addObject:AVCaptureDeviceTypeContinuityCamera];
  }
  AVCaptureDeviceDiscoverySession *session =
      [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:types
                                mediaType:AVMediaTypeVideo
                                 position:AVCaptureDevicePositionUnspecified];
  return session.devices;
}

AVCaptureDevice *select_device(const std::string &id) {
  NSArray<AVCaptureDevice *> *devices = enumerate_devices();
  if (devices.count == 0) {
    return nil;
  }
  if (id.empty() || id == "default" || id == "video0") {
    return devices[0];
  }
  int index = -1;
  if (parse_index(id, index)) {
    if (index >= 0 && index < static_cast<int>(devices.count)) {
      return devices[static_cast<NSUInteger>(index)];
    }
  }
  NSString *target = [NSString stringWithUTF8String:id.c_str()];
  for (AVCaptureDevice *device in devices) {
    if ([device.uniqueID isEqualToString:target] ||
        [device.localizedName isEqualToString:target]) {
      return device;
    }
  }
  return devices[0];
}

AVCaptureDeviceFormat *best_format(AVCaptureDevice *device, int width,
                                   int height) {
  AVCaptureDeviceFormat *best = nil;
  int best_score = INT32_MAX;
  for (AVCaptureDeviceFormat *format in device.formats) {
    CMFormatDescriptionRef desc = format.formatDescription;
    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(desc);
    if (dims.width <= 0 || dims.height <= 0)
      continue;
    int score = std::abs(dims.width - width) + std::abs(dims.height - height);
    if (score < best_score) {
      best_score = score;
      best = format;
    }
  }
  return best;
}
} // namespace

std::vector<std::string> list_avfoundation_devices() {
  std::vector<std::string> devices_out;
  @autoreleasepool {
    NSArray<AVCaptureDevice *> *devices = enumerate_devices();
    int idx = 0;
    for (AVCaptureDevice *device in devices) {
      (void)device;
      devices_out.push_back("video" + std::to_string(idx));
      ++idx;
    }
  }
  return devices_out;
}

@interface FrameDelegate
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) CaptureV4L2 *owner;
- (instancetype)initWithOwner:(CaptureV4L2 *)owner;
@end

@implementation FrameDelegate
- (instancetype)initWithOwner:(CaptureV4L2 *)owner {
  if ((self = [super init])) {
    _owner = owner;
  }
  return self;
}

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection {
  (void)output;
  (void)connection;
  if (_owner) {
    _owner->handle_sample(reinterpret_cast<void *>(sampleBuffer));
  }
}
@end

struct CaptureV4L2::Impl {
  AVCaptureSession *session = nil;
  AVCaptureDeviceInput *input = nil;
  AVCaptureVideoDataOutput *output = nil;
  FrameDelegate *delegate = nil;
  dispatch_queue_t queue = nullptr;
  CIContext *ci_context = nil;
  bool want_mjpeg = false;
};

CaptureV4L2::CaptureV4L2() = default;

CaptureV4L2::~CaptureV4L2() { stop(); }

bool CaptureV4L2::start(const std::string &device_id,
                        const CaptureParams &params) {
  if (running_)
    return true;
  device_id_ = device_id;
  params_ = params;

  @autoreleasepool {
    AVCaptureDevice *device = select_device(device_id_);
    if (!device) {
      std::cerr << "No AVFoundation video device available\n";
      return false;
    }

    NSError *error = nil;
    AVCaptureDeviceInput *input =
        [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (!input) {
      std::cerr << "Failed to create device input: "
                << (error ? error.localizedDescription.UTF8String : "unknown")
                << "\n";
      return false;
    }

    AVCaptureSession *session = [[AVCaptureSession alloc] init];
    if ([session canSetSessionPreset:AVCaptureSessionPresetHigh]) {
      session.sessionPreset = AVCaptureSessionPresetHigh;
    }

    if ([device lockForConfiguration:&error]) {
      AVCaptureDeviceFormat *format =
          best_format(device, params_.width, params_.height);
      if (format) {
        device.activeFormat = format;
      }
      if (params_.fps > 0) {
        CMTime frame_duration = CMTimeMake(1, params_.fps);
        device.activeVideoMinFrameDuration = frame_duration;
        device.activeVideoMaxFrameDuration = frame_duration;
      }
      [device unlockForConfiguration];
    } else if (error) {
      std::cerr << "Failed to lock device for configuration: "
                << error.localizedDescription.UTF8String << "\n";
    }

    if ([session canAddInput:input]) {
      [session addInput:input];
    } else {
      std::cerr << "Cannot add capture input\n";
      return false;
    }

    AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
    bool want_mjpeg = (params_.codec == "mjpeg");
    NSDictionary *settings = @{
      (id)kCVPixelBufferPixelFormatTypeKey :
          @(want_mjpeg ? kCVPixelFormatType_32BGRA
                       : kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
    };
    output.videoSettings = settings;
    output.alwaysDiscardsLateVideoFrames = YES;

    FrameDelegate *delegate = [[FrameDelegate alloc] initWithOwner:this];
    dispatch_queue_t queue =
        dispatch_queue_create("silkcast.capture", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:delegate queue:queue];

    if ([session canAddOutput:output]) {
      [session addOutput:output];
    } else {
      std::cerr << "Cannot add capture output\n";
      return false;
    }

    [session startRunning];

    if (!impl_) {
      impl_ = std::make_unique<Impl>();
    }
    impl_->session = session;
    impl_->input = input;
    impl_->output = output;
    impl_->delegate = delegate;
    impl_->queue = queue;
    impl_->ci_context = [CIContext contextWithOptions:nil];
    impl_->want_mjpeg = want_mjpeg;

    CMVideoDimensions dims =
        CMVideoFormatDescriptionGetDimensions(device.activeFormat
                                                  .formatDescription);
    if (dims.width > 0 && dims.height > 0) {
      params_.width = dims.width;
      params_.height = dims.height;
    }

    pixel_format_ = want_mjpeg ? PixelFormat::MJPEG : PixelFormat::NV12;
    running_ = true;
    return true;
  }
}

void CaptureV4L2::stop() {
  if (!running_ && !impl_)
    return;
  running_ = false;
  if (impl_) {
    if (impl_->output && impl_->delegate) {
      impl_->delegate.owner = nullptr;
      [impl_->output setSampleBufferDelegate:nil queue:nullptr];
    }
    if (impl_->session) {
      [impl_->session stopRunning];
    }
    impl_.reset();
  }
  std::lock_guard<std::mutex> lock(buf_mu_);
  buffer_.clear();
}

bool CaptureV4L2::latest_frame(std::string &out) {
  std::lock_guard<std::mutex> lock(buf_mu_);
  if (buffer_.empty())
    return false;
  out = buffer_;
  return true;
}

void CaptureV4L2::handle_sample(void *sample_buffer) {
  if (!running_ || !impl_)
    return;
  @autoreleasepool {
    auto *sample = reinterpret_cast<CMSampleBufferRef>(sample_buffer);
    CVImageBufferRef image_buffer = CMSampleBufferGetImageBuffer(sample);
    if (!image_buffer)
      return;
    CVPixelBufferLockBaseAddress(image_buffer, kCVPixelBufferLock_ReadOnly);
    const size_t width = CVPixelBufferGetWidth(image_buffer);
    const size_t height = CVPixelBufferGetHeight(image_buffer);

    if (impl_->want_mjpeg) {
      if (!impl_->ci_context) {
        impl_->ci_context = [CIContext contextWithOptions:nil];
      }
      CIImage *ci_image = [CIImage imageWithCVImageBuffer:image_buffer];
      CGRect rect = CGRectMake(0, 0, width, height);
      CGImageRef cg_image = [impl_->ci_context createCGImage:ci_image
                                                    fromRect:rect];
      if (cg_image) {
        NSMutableData *data = [NSMutableData data];
        CGImageDestinationRef dest =
            CGImageDestinationCreateWithData((CFMutableDataRef)data,
                                             CFSTR("public.jpeg"), 1, NULL);
        if (dest) {
          NSDictionary *props = @{
            (id)kCGImageDestinationLossyCompressionQuality : @(0.8)
          };
          CGImageDestinationAddImage(dest, cg_image,
                                     (__bridge CFDictionaryRef)props);
          CGImageDestinationFinalize(dest);
          CFRelease(dest);
          std::lock_guard<std::mutex> lock(buf_mu_);
          buffer_.assign(reinterpret_cast<const char *>(data.bytes),
                         data.length);
        }
        CGImageRelease(cg_image);
      }
    } else {
      if (CVPixelBufferGetPlaneCount(image_buffer) < 2) {
        CVPixelBufferUnlockBaseAddress(image_buffer,
                                       kCVPixelBufferLock_ReadOnly);
        return;
      }
      const uint8_t *src_y = reinterpret_cast<const uint8_t *>(
          CVPixelBufferGetBaseAddressOfPlane(image_buffer, 0));
      const uint8_t *src_uv = reinterpret_cast<const uint8_t *>(
          CVPixelBufferGetBaseAddressOfPlane(image_buffer, 1));
      const size_t stride_y =
          CVPixelBufferGetBytesPerRowOfPlane(image_buffer, 0);
      const size_t stride_uv =
          CVPixelBufferGetBytesPerRowOfPlane(image_buffer, 1);
      const size_t y_size = width * height;
      const size_t uv_size = y_size / 2;
      std::string local;
      local.resize(y_size + uv_size);
      uint8_t *dst = reinterpret_cast<uint8_t *>(local.data());
      uint8_t *dst_y = dst;
      uint8_t *dst_uv = dst + y_size;
      for (size_t y = 0; y < height; ++y) {
        std::memcpy(dst_y + y * width, src_y + y * stride_y, width);
      }
      for (size_t y = 0; y < height / 2; ++y) {
        std::memcpy(dst_uv + y * width, src_uv + y * stride_uv, width);
      }
      std::lock_guard<std::mutex> lock(buf_mu_);
      buffer_ = std::move(local);
    }

    CVPixelBufferUnlockBaseAddress(image_buffer, kCVPixelBufferLock_ReadOnly);
  }
}

#endif // __APPLE__
