#pragma once

#include <QString>

#include <functional>
#include <thread>

struct webrtc_AdaptedVideoTrackSource;

namespace evp {

class DesktopCapture {
public:
    ~DesktopCapture();

    // Start only after local consent. Keep source alive until stop returns.
    // Callbacks run on the capture thread. Schedule teardown on the owner thread.
    bool start(webrtc_AdaptedVideoTrackSource* source, int maxFps,
               std::function<void(QString)> onError,
               std::function<void(int, int)> onSize = {});
    void stop();

private:
    std::jthread worker_;
};

}  // namespace evp
