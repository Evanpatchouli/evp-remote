#pragma once

#include <QString>

#include <functional>
#include <memory>

struct webrtc_VideoEncoderFactory_unique;

namespace evp {

class SendQueuePressure;

// Callbacks run on the WebRTC encoder sequence. Schedule teardown on the owner thread.
webrtc_VideoEncoderFactory_unique* createFfmpegEncoderFactory(
    std::function<void(QString)> onError = {}, std::function<void(QString)> onBackend = {},
    std::shared_ptr<SendQueuePressure> sendPressure = {});

}  // namespace evp
