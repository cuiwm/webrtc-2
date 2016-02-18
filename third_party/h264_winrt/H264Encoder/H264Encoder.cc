/*
*  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
*
*  Use of this source code is governed by a BSD-style license
*  that can be found in the LICENSE file in the root of the source
*  tree. An additional intellectual property rights grant can be found
*  in the file PATENTS.  All contributing project authors may
*  be found in the AUTHORS file in the root of the source tree.
*/

#include "third_party/h264_winrt/H264Encoder/H264Encoder.h"

#include <Windows.h>
#include <stdlib.h>
#include <ppltasks.h>
#include <mfapi.h>
#include <robuffer.h>
#include <wrl.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl\implements.h>
#include <codecapi.h>
#include <sstream>
#include <vector>
#include <iomanip>

#include "H264StreamSink.h"
#include "H264MediaSink.h"
#include "Utils/Utils.h"
#include "webrtc/modules/video_coding/codecs/interface/video_codec_interface.h"
#include "libyuv/convert.h"
#include "webrtc/base/logging.h"


#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid.lib")

namespace webrtc {

//////////////////////////////////////////
// H264 WinRT Encoder Implementation
//////////////////////////////////////////

H264WinRTEncoderImpl::H264WinRTEncoderImpl()
  : _lock(webrtc::CriticalSectionWrapper::CreateCriticalSection())
  , firstFrame_(true)
  , startTime_(0)
  , framePendingCount_(0)
  , frameCount_(0)
  , lastFrameDropped_(false)
  , lastTimestampHns_(0) {
}

H264WinRTEncoderImpl::~H264WinRTEncoderImpl() {
  Release();
}

int H264WinRTEncoderImpl::InitEncode(const VideoCodec* inst,
  int number_of_cores,
  size_t /*maxPayloadSize */) {
  HRESULT hr = S_OK;

  webrtc::CriticalSectionScoped csLock(_lock.get());

  ON_SUCCEEDED(MFStartup(MF_VERSION));

  // output media type (h264)
  ON_SUCCEEDED(MFCreateMediaType(&mediaTypeOut_));
  ON_SUCCEEDED(mediaTypeOut_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
  ON_SUCCEEDED(mediaTypeOut_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
  // TODO(winrt): Lumia 635 and Lumia 1520 Windows phones don't work well
  //              with constrained baseline profile. Uncomment or delete
  //              the line below as soon as we find the reason why.
  // ON_SUCCEEDED(mediaTypeOut_->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_ConstrainedBase));

  // Weight*Height*2 kbit represents a good balance between video quality and
  // the bandwidth that a 620 Windows phone can handle.
  ON_SUCCEEDED(mediaTypeOut_->SetUINT32(MF_MT_AVG_BITRATE,
      inst->targetBitrate > 0 ? inst->targetBitrate : inst->height * inst->width * 2.0));
  ON_SUCCEEDED(mediaTypeOut_->SetUINT32(MF_MT_INTERLACE_MODE,
    MFVideoInterlace_Progressive));
  ON_SUCCEEDED(MFSetAttributeSize(mediaTypeOut_.Get(),
    MF_MT_FRAME_SIZE, inst->width, inst->height));
  ON_SUCCEEDED(MFSetAttributeRatio(mediaTypeOut_.Get(),
    MF_MT_FRAME_RATE, inst->maxFramerate, 1));

  // input media type (nv12)
  ON_SUCCEEDED(MFCreateMediaType(&mediaTypeIn_));
  ON_SUCCEEDED(mediaTypeIn_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
  ON_SUCCEEDED(mediaTypeIn_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
  ON_SUCCEEDED(mediaTypeIn_->SetUINT32(
    MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
  ON_SUCCEEDED(mediaTypeIn_->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
  ON_SUCCEEDED(MFSetAttributeSize(mediaTypeIn_.Get(),
    MF_MT_FRAME_SIZE, inst->width, inst->height));
  ON_SUCCEEDED(MFSetAttributeRatio(mediaTypeIn_.Get(),
    MF_MT_FRAME_RATE, inst->maxFramerate, 1));

  quality_scaler_.Init(inst->qpMax / QualityScaler::kDefaultLowQpDenominator, 64, false);
  quality_scaler_.ReportFramerate(inst->maxFramerate);

  // Create the media sink
  ON_SUCCEEDED(Microsoft::WRL::MakeAndInitialize<H264MediaSink>(&mediaSink_));

  // SinkWriter creation attributes
  ON_SUCCEEDED(MFCreateAttributes(&sinkWriterCreationAttributes_, 1));
  ON_SUCCEEDED(sinkWriterCreationAttributes_->SetUINT32(
    MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
  ON_SUCCEEDED(sinkWriterCreationAttributes_->SetUINT32(
    MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
  ON_SUCCEEDED(sinkWriterCreationAttributes_->SetUINT32(
    MF_LOW_LATENCY, TRUE));

  // Create the sink writer
  ON_SUCCEEDED(MFCreateSinkWriterFromMediaSink(mediaSink_.Get(),
    sinkWriterCreationAttributes_.Get(), &sinkWriter_));

  // Add the h264 output stream to the writer
  ON_SUCCEEDED(sinkWriter_->AddStream(mediaTypeOut_.Get(), &streamIndex_));

  // SinkWriter encoder properties
  ON_SUCCEEDED(MFCreateAttributes(&sinkWriterEncoderAttributes_, 1));
  ON_SUCCEEDED(sinkWriter_->SetInputMediaType(streamIndex_, mediaTypeIn_.Get(),
    sinkWriterEncoderAttributes_.Get()));

  // Register this as the callback for encoded samples.
  ON_SUCCEEDED(mediaSink_->RegisterEncodingCallback(this));

  ON_SUCCEEDED(sinkWriter_->BeginWriting());

  if (SUCCEEDED(hr)) {
    inited_ = true;
    return WEBRTC_VIDEO_CODEC_OK;
  } else {
    return hr;
  }
}

int H264WinRTEncoderImpl::RegisterEncodeCompleteCallback(
  EncodedImageCallback* callback) {
  webrtc::CriticalSectionScoped csLock(_lock.get());
  encodedCompleteCallback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int H264WinRTEncoderImpl::Release() {
  // Use a temporary sink variable to prevent lock inversion
  // between the shutdown call and OnH264Encoded() callback.
  ComPtr<H264MediaSink> tmpMediaSink;

  {
    webrtc::CriticalSectionScoped csLock(_lock.get());
    sinkWriter_.Reset();
    if (mediaSink_ != nullptr) {
      tmpMediaSink = mediaSink_;
    }
    sinkWriterCreationAttributes_.Reset();
    sinkWriterEncoderAttributes_.Reset();
    mediaTypeOut_.Reset();
    mediaTypeIn_.Reset();
    mediaSink_.Reset();
    encodedCompleteCallback_ = nullptr;
    startTime_ = 0;
    lastTimestampHns_ = 0;
    firstFrame_ = true;
    inited_ = false;
    framePendingCount_ = 0;
    _sampleAttributeQueue.clear();
  }

  if (tmpMediaSink != nullptr) {
    tmpMediaSink->Shutdown();
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

//#define DYNAMIC_SCALING

ComPtr<IMFSample> H264WinRTEncoderImpl::FromVideoFrame(const VideoFrame& frame) {
  HRESULT hr = S_OK;
  ComPtr<IMFSample> sample;
  ON_SUCCEEDED(MFCreateSample(sample.GetAddressOf()));

  ComPtr<IMFAttributes> sampleAttributes;
  ON_SUCCEEDED(sample.As(&sampleAttributes));

  quality_scaler_.OnEncodeFrame(frame);
#ifdef DYNAMIC_SCALING
  const VideoFrame& dstFrame = quality_scaler_.GetScaledFrame(frame, 16);
#else
  const VideoFrame& dstFrame = frame;
#endif

  if (SUCCEEDED(hr)) {
    auto totalSize = dstFrame.allocated_size(PlaneType::kYPlane) +
      dstFrame.allocated_size(PlaneType::kUPlane) +
      dstFrame.allocated_size(PlaneType::kVPlane);

    ComPtr<IMFMediaBuffer> mediaBuffer;
    ON_SUCCEEDED(MFCreateMemoryBuffer(totalSize, mediaBuffer.GetAddressOf()));

    BYTE* destBuffer = nullptr;
    if (SUCCEEDED(hr)) {
      DWORD cbMaxLength;
      DWORD cbCurrentLength;
      ON_SUCCEEDED(mediaBuffer->Lock(
        &destBuffer, &cbMaxLength, &cbCurrentLength));
    }

    if (SUCCEEDED(hr)) {
      BYTE* destUV = destBuffer +
        (dstFrame.stride(PlaneType::kYPlane) * dstFrame.height());
      libyuv::I420ToNV12(
        dstFrame.buffer(PlaneType::kYPlane), dstFrame.stride(PlaneType::kYPlane),
        dstFrame.buffer(PlaneType::kUPlane), dstFrame.stride(PlaneType::kUPlane),
        dstFrame.buffer(PlaneType::kVPlane), dstFrame.stride(PlaneType::kVPlane),
        destBuffer, dstFrame.stride(PlaneType::kYPlane),
        destUV, dstFrame.stride(PlaneType::kYPlane),
        dstFrame.width(),
        dstFrame.height());
    }

    {
      ComPtr<IMFSinkWriterEx> sinkWriterEx;
      sinkWriter_.As(&sinkWriterEx);
      ComPtr<IMFTransform> transform;
      GUID transformCategory;
      ON_SUCCEEDED(sinkWriterEx->GetTransformForStream(streamIndex_, 0, &transformCategory, &transform));

      UINT32 currentWidth, currentHeight;
      MFGetAttributeSize(mediaTypeOut_.Get(),
        MF_MT_FRAME_SIZE, &currentWidth, &currentHeight);

      if (dstFrame.width() != (int)currentWidth || dstFrame.height() != (int)currentHeight) {
        MFSetAttributeSize(mediaTypeOut_.Get(), MF_MT_FRAME_SIZE, dstFrame.width(), dstFrame.height());
        MFSetAttributeSize(mediaTypeIn_.Get(), MF_MT_FRAME_SIZE, dstFrame.width(), dstFrame.height());

        ON_SUCCEEDED(transform->SetInputType(0, nullptr, 0));
        ON_SUCCEEDED(transform->SetOutputType(0, nullptr, 0));
        ON_SUCCEEDED(transform->SetOutputType(0, mediaTypeOut_.Get(), 0));
        ON_SUCCEEDED(transform->SetInputType(0, mediaTypeIn_.Get(), 0));
        OutputDebugString((L"H264WinRTDecoder: resolution updated: " +
          dstFrame.width().ToString() + L"x" +
          dstFrame.height().ToString() + L"\r\n")->Data());
        LOG(LS_WARNING) << "Resolution updated: " <<
          dstFrame.width() << "x" << dstFrame.height();
      }
    }

    if (firstFrame_) {
      firstFrame_ = false;
      startTime_ = dstFrame.timestamp();
    }

    auto timestampHns = ((dstFrame.timestamp() - startTime_) / 90) * 1000 * 10;
    ON_SUCCEEDED(sample->SetSampleTime(timestampHns));

    if (SUCCEEDED(hr)) {
      auto durationHns = timestampHns - lastTimestampHns_;
      hr = sample->SetSampleDuration(durationHns);
    }

    if (SUCCEEDED(hr)) {
      lastTimestampHns_ = timestampHns;

      // Cache the frame attributes to get them back after the encoding.
      CachedFrameAttributes frameAttributes;
      frameAttributes.timestamp = dstFrame.timestamp();
      frameAttributes.ntpTime = dstFrame.ntp_time_ms();
      frameAttributes.captureRenderTime = dstFrame.render_time_ms();
      frameAttributes.frameWidth = dstFrame.width();
      frameAttributes.frameHeight = dstFrame.height();
      _sampleAttributeQueue.push(timestampHns, frameAttributes);
    }

    ON_SUCCEEDED(mediaBuffer->SetCurrentLength(
      dstFrame.width() * dstFrame.height() * 3 / 2));

    if (destBuffer != nullptr) {
      mediaBuffer->Unlock();
    }

    ON_SUCCEEDED(sample->AddBuffer(mediaBuffer.Get()));

    if (lastFrameDropped_) {
      lastFrameDropped_ = false;
      sampleAttributes->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    }
  }

  return sample;
}

int H264WinRTEncoderImpl::Encode(
  const VideoFrame& frame,
  const CodecSpecificInfo* codec_specific_info,
  const std::vector<FrameType>* frame_types) {
  {
    webrtc::CriticalSectionScoped csLock(_lock.get());
    if (!inited_) {
      return -1;
    }
  }

  HRESULT hr = S_OK;

  codecSpecificInfo_ = codec_specific_info;

  {
    webrtc::CriticalSectionScoped csLock(_lock.get());

    auto sample = FromVideoFrame(frame);

    ON_SUCCEEDED(sinkWriter_->WriteSample(streamIndex_, sample.Get()));

    ++frameCount_;
    if (frameCount_ % 30 == 0) {
      ON_SUCCEEDED(sinkWriter_->NotifyEndOfSegment(streamIndex_));
    }

    ++framePendingCount_;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

void H264WinRTEncoderImpl::OnH264Encoded(ComPtr<IMFSample> sample) {
  {
    webrtc::CriticalSectionScoped csLock(_lock.get());
    if (!inited_ || encodedCompleteCallback_ == nullptr) {
      return;
    }
    --framePendingCount_;
  }

  DWORD totalLength;
  HRESULT hr = S_OK;
  ON_SUCCEEDED(sample->GetTotalLength(&totalLength));

  ComPtr<IMFMediaBuffer> buffer;
  hr = sample->GetBufferByIndex(0, &buffer);

  if (SUCCEEDED(hr)) {
    BYTE* byteBuffer;
    DWORD maxLength;
    DWORD curLength;
    hr = buffer->Lock(&byteBuffer, &maxLength, &curLength);
    if (FAILED(hr)) {
      return;
    }
    if (curLength == 0) {
      LOG(LS_WARNING) << "Got empty sample.";
      buffer->Unlock();
      return;
    }
    std::vector<byte> sendBuffer;
    sendBuffer.resize(curLength);
    memcpy(sendBuffer.data(), byteBuffer, curLength);
    hr = buffer->Unlock();
    if (FAILED(hr)) {
      return;
    }

    // sendBuffer is not copied here.
    EncodedImage encodedImage(sendBuffer.data(), curLength, curLength);

    ComPtr<IMFAttributes> sampleAttributes;
    hr = sample.As(&sampleAttributes);
    if (SUCCEEDED(hr)) {
      UINT32 cleanPoint;
      hr = sampleAttributes->GetUINT32(
        MFSampleExtension_CleanPoint, &cleanPoint);
      if (SUCCEEDED(hr) && cleanPoint) {
        encodedImage._completeFrame = true;
        encodedImage._frameType = kVideoFrameKey;
      }
    }

    _h264Parser.ParseBitstream(sendBuffer.data(), sendBuffer.size());
    int lastQp;
    if (_h264Parser.GetLastSliceQp(&lastQp)) {
      quality_scaler_.ReportQP(lastQp);
    } else {
      OutputDebugString(L"H264WinRTDecoder: Couldn't find QP\r\n");
    }

    // Scan for and create mark all fragments.
    RTPFragmentationHeader fragmentationHeader;
    uint32_t fragIdx = 0;
    for (uint32_t i = 0; i < sendBuffer.size() - 5; ++i) {
      byte* ptr = sendBuffer.data() + i;
      int prefixLengthFound = 0;
      if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x00 && ptr[3] == 0x01
        && ((ptr[4] & 0x1f) != 0x09 /* ignore access unit delimiters */)) {
        prefixLengthFound = 4;
      } else if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x01
        && ((ptr[3] & 0x1f) != 0x09 /* ignore access unit delimiters */)) {
        prefixLengthFound = 3;
      }

      // Found a key frame, mark is as such in case
      // MFSampleExtension_CleanPoint wasn't set on the sample.
      if (prefixLengthFound > 0 && (ptr[prefixLengthFound] & 0x1f) == 0x05) {
        encodedImage._completeFrame = true;
        encodedImage._frameType = kVideoFrameKey;
      }

      if (prefixLengthFound > 0) {
        fragmentationHeader.VerifyAndAllocateFragmentationHeader(fragIdx + 1);
        fragmentationHeader.fragmentationOffset[fragIdx] = i + prefixLengthFound;
        fragmentationHeader.fragmentationLength[fragIdx] = 0;  // We'll set that later
        // Set the length of the previous fragment.
        if (fragIdx > 0) {
          fragmentationHeader.fragmentationLength[fragIdx - 1] =
            i - fragmentationHeader.fragmentationOffset[fragIdx - 1];
        }
        fragmentationHeader.fragmentationPlType[fragIdx] = 0;
        fragmentationHeader.fragmentationTimeDiff[fragIdx] = 0;
        ++fragIdx;
        i += 5;
      }
    }
    // Set the length of the last fragment.
    if (fragIdx > 0) {
      fragmentationHeader.fragmentationLength[fragIdx - 1] =
        sendBuffer.size() -
        fragmentationHeader.fragmentationOffset[fragIdx - 1];
    }

    {
      webrtc::CriticalSectionScoped csLock(_lock.get());

      LONGLONG sampleTimestamp;
      sample->GetSampleTime(&sampleTimestamp);

      CachedFrameAttributes frameAttributes;
      if (_sampleAttributeQueue.pop(sampleTimestamp, frameAttributes)) {
        encodedImage._timeStamp = frameAttributes.timestamp;
        encodedImage.ntp_time_ms_ = frameAttributes.ntpTime;
        encodedImage.capture_time_ms_ = frameAttributes.captureRenderTime;
        encodedImage._encodedWidth = frameAttributes.frameWidth;
        encodedImage._encodedHeight = frameAttributes.frameHeight;
        encodedImage.adapt_reason_.quality_resolution_downscales =
          quality_scaler_.downscale_shift();
      }

      if (encodedCompleteCallback_ != nullptr) {
        encodedCompleteCallback_->Encoded(
          encodedImage, codecSpecificInfo_, &fragmentationHeader);
      }
    }
  }
}

int H264WinRTEncoderImpl::SetChannelParameters(
  uint32_t packetLoss, int64_t rtt) {
  return WEBRTC_VIDEO_CODEC_OK;
}

#define DYNAMIC_FPS
#define DYNAMIC_BITRATE

int H264WinRTEncoderImpl::SetRates(
  uint32_t new_bitrate_kbit, uint32_t new_framerate) {
  // This may happen. Ignore it.
  if (new_framerate == 0) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  webrtc::CriticalSectionScoped csLock(_lock.get());
  if (sinkWriter_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  HRESULT hr = S_OK;

  uint32_t old_bitrate_kbit, old_framerate, one;

  hr = mediaTypeOut_->GetUINT32(MF_MT_AVG_BITRATE, &old_bitrate_kbit);
  old_bitrate_kbit /= 1024;
  hr = MFGetAttributeRatio(mediaTypeOut_.Get(),
    MF_MT_FRAME_RATE, &old_framerate, &one);

  if (old_bitrate_kbit != new_bitrate_kbit) {
    LOG(LS_INFO) << "H264WinRTEncoder: SetRates "
      << new_bitrate_kbit << "kbit " << new_framerate << "fps";
    OutputDebugString((L"H264WinRTEncoder: SetRates " +
      new_bitrate_kbit.ToString() + L"kbps, " +
      new_framerate + L"fps\r\n")->Data());

    bool bitrateUpdated = false;
    bool fpsUpdated = false;
#ifdef DYNAMIC_BITRATE
    hr = mediaTypeOut_->SetUINT32(MF_MT_AVG_BITRATE, new_bitrate_kbit * 1024);
    bitrateUpdated = true;
#endif

#ifdef DYNAMIC_FPS
    if (old_framerate != new_framerate) {
      hr = MFSetAttributeRatio(mediaTypeOut_.Get(), MF_MT_FRAME_RATE, new_framerate, 1);
      hr = MFSetAttributeRatio(mediaTypeIn_.Get(), MF_MT_FRAME_RATE, new_framerate, 1);
      fpsUpdated = true;
    }
#endif
    quality_scaler_.ReportFramerate(new_framerate);

    if (bitrateUpdated || fpsUpdated) {
      ComPtr<IMFSinkWriterEx> sinkWriterEx;
      sinkWriter_.As(&sinkWriterEx);
      ComPtr<IMFTransform> transform;
      GUID transformCategory = { 0 };
      int transformIndex = 0;
      do
      {
        ON_SUCCEEDED(sinkWriterEx->GetTransformForStream(streamIndex_, transformIndex, &transformCategory, &transform));
        OutputDebugString((L"GetTransformForStream returned guid: " + transformCategory.Data1.ToString() + L"\n")->Data());
        transformIndex++;
      } while (SUCCEEDED(hr) && transformCategory != MFT_CATEGORY_VIDEO_ENCODER && transform != nullptr);
      // MFT_CATEGORY_AUDIO_DECODER
      if (transform.Get() == nullptr || transformCategory != MFT_CATEGORY_VIDEO_ENCODER) {
        OutputDebugString(L"GetTransformForStream() couldn't find transform.\n");
        LOG(LS_WARNING) << "GetTransformForStream() couldn't find transform";
        return WEBRTC_VIDEO_CODEC_OK;
      }

      ComPtr<IMFSinkWriterEncoderConfig> encoderConfig;
      sinkWriter_.As(&encoderConfig);

      if (false) {
        // For now, we don't use these functions.
        // They cause serious issues on every devices I test with except Surface Book.
        // TODO(winrt): Detect programatically the Surface Book and use these APIs.
        ON_SUCCEEDED(encoderConfig->SetTargetMediaType(0, mediaTypeOut_.Get(), nullptr));
        ON_SUCCEEDED(sinkWriter_->SetInputMediaType(0, mediaTypeIn_.Get(), nullptr));
      }
      else {
        // This works extremely smoothly accross all devices
        // except Surface Book.
        ON_SUCCEEDED(transform->SetOutputType(0, mediaTypeOut_.Get(), 0));
        ON_SUCCEEDED(transform->SetInputType(0, mediaTypeIn_.Get(), 0));
      }
    }
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

void H264WinRTEncoderImpl::OnDroppedFrame(uint32_t timestamp) {
  webrtc::CriticalSectionScoped csLock(_lock.get());
  quality_scaler_.ReportDroppedFrame();
  auto timestampHns = ((timestamp - startTime_) / 90) * 1000 * 10;
  sinkWriter_->SendStreamTick(streamIndex_, timestampHns);
  lastFrameDropped_ = true;
}

}  // namespace webrtc
