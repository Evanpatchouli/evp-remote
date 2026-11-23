#pragma once

#include "../rtc/cursor_protocol.h"

#include <QString>

#include <atomic>
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
               std::function<void(int, int)> onSize = {},
               std::function<void(quint64, CursorSnapshot)> onCursor = {});
    void setMaxFps(int maxFps);
    void setCursorEpoch(quint64 epoch);
    void noteCursorMove(quint64 epoch, quint32 serial, QPointF position);
    void stop();

private:
    struct CursorMove {
        quint64 epoch = 0;
        quint32 serial = 0;
        QPointF position;
        bool matched = false;
    };

    void run(std::stop_token stop, webrtc_AdaptedVideoTrackSource* source,
             const std::function<void(int, int)>& onSize,
             const std::function<void(quint64, CursorSnapshot)>& onCursor);
    quint32 appliedCursorMove(quint64 epoch, const CursorSnapshot& state);

    std::mutex cadenceMutex_;
    std::condition_variable_any cadenceChanged_;
    int maxFps_ = 60;
    std::jthread worker_;
    std::atomic<quint64> cursorEpoch_{0};
    std::mutex cursorMoveMutex_;
    CursorMove cursorMove_;
};

}  // namespace evp
