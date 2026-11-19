#pragma once

#include "controller.h"
#include <atomic>
#include <mutex>
#include <utility>

namespace evp {
// The SDK owns its threads. Invalidate queued work before either role releases its UI or devices.
struct DesktopController::Dispatch {
    std::mutex lock;
    DesktopController* target;
    VideoSurface* surface;
    quint64 generation;
    std::atomic<quint64> gpuDecoded{0}, cpuDecoded{0}, inputGrant{0};
    Dispatch(DesktopController* c, VideoSurface* v, quint64 g) : target(c), surface(v), generation(g) {}
    template <class F> void post(F work) {
        std::lock_guard guard(lock);
        if (!target)
            return;
        QMetaObject::invokeMethod(
            target,
            [c = target, g = generation, work = std::move(work)]() mutable {
                if (c->generation_ == g)
                    work(*c);
            },
            Qt::QueuedConnection);
    }
    void frame(const I420Frame&);
    void decoded(const DecodedVideoFrame&, int rotation);
    void detach() {
        std::lock_guard guard(lock);
        inputGrant = 0;
        target = nullptr;
        surface = nullptr;
    }
};
} // namespace evp
