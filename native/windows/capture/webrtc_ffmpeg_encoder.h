#pragma once

#include <QString>

#include <functional>

struct webrtc_VideoEncoderFactory_unique;

namespace evp {

// Callbacks run on the WebRTC encoder sequence. Schedule teardown on the owner thread.
webrtc_VideoEncoderFactory_unique* createFfmpegEncoderFactory(
    std::function<void(QString)> onError = {}, std::function<void(QString)> onBackend = {});

}  // namespace evp
