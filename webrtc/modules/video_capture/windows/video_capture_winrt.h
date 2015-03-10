#ifndef WEBRTC_MODULES_VIDEO_CAPTURE_WINDOWS_VIDEO_CAPTURE_WINRT_H_
#define WEBRTC_MODULES_VIDEO_CAPTURE_WINDOWS_VIDEO_CAPTURE_WINRT_H_

#include "webrtc/modules/video_capture/video_capture_impl.h"

namespace webrtc {
namespace videocapturemodule {

class VideoCaptureWinRT : public VideoCaptureImpl {
 public:
  explicit VideoCaptureWinRT(const int32_t id);

  int32_t Init(const int32_t id, const char* device_id);

  // Overrides from VideoCaptureImpl.
  virtual int32_t StartCapture(const VideoCaptureCapability& capability);
  virtual int32_t StopCapture();
  virtual bool CaptureStarted();
  virtual int32_t CaptureSettings(
      VideoCaptureCapability& settings);

 protected:
  virtual ~VideoCaptureWinRT();
};

}  // namespace videocapturemodule
}  // namespace webrtc

#endif  // WEBRTC_MODULES_VIDEO_CAPTURE_WINDOWS_VIDEO_CAPTURE_WINRT_H_