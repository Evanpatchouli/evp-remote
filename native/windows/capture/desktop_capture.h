#pragma once

#include <QString>

#include <condition_variable>
#include <functional>
#include <mutex>
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
    void setMaxFps(int maxFps);
    void stop();

private:
    void run(std::stop_token stop, webrtc_AdaptedVideoTrackSource* source,
             const std::function<void(int, int)>& onSize);

    std::mutex cadenceMutex_;
    std::condition_variable_any cadenceChanged_;
    int maxFps_ = 60;
    std::jthread worker_;
};

}  // namespace evp
