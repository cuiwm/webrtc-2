# Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
#
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file in the root of the source
# tree. An additional intellectual property rights grant can be found
# in the file PATENTS.  All contributing project authors may
# be found in the AUTHORS file in the root of the source tree.

{
  'includes': [
    '../../webrtc/build/common.gypi',
  ],
  'targets': [
    {
      'target_name': 'webrtc_h264_winrt',
      'type': 'static_library',
      'dependencies': [
        '<(webrtc_root)/common.gyp:webrtc_common',
        '<(webrtc_root)/common_video/common_video.gyp:common_video',
        '<(webrtc_root)/modules/video_coding/utility/video_coding_utility.gyp:video_coding_utility',
        '<(webrtc_root)/system_wrappers/system_wrappers.gyp:system_wrappers',
      ],
      'sources': [
        'h264_winrt_factory.cc',
        'h264_winrt_factory.h',

        'Utils/Utils.h',
        'Utils/Async.h',
        'Utils/ComPtrList.h',
        'Utils/CritSec.h',
        'Utils/OpQueue.h',

        'H264Encoder/H264Encoder.h',
        'H264Encoder/H264Encoder.cc',
        'H264Encoder/H264MediaSink.h',
        'H264Encoder/H264MediaSink.cc',
        'H264Encoder/H264StreamSink.h',
        'H264Encoder/H264StreamSink.cc',
        'H264Encoder/IH264EncodingCallback.h',

        'H264Decoder/H264Decoder.h',
        'H264Decoder/H264Decoder.cc',
        'H264Decoder/H264MediaSource.h',
        'H264Decoder/H264MediaSource.cc',
        'H264Decoder/H264MediaStream.h',
        'H264Decoder/H264MediaStream.cc',
        'H264Decoder/SourceReaderCB.h',
        'H264Decoder/SourceReaderCB.cc',
        'H264Decoder/IH264DecodingCallback.h',
      ],
    },
  ], # targets
}