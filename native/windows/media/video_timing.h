#pragma once

#include <QLoggingCategory>
#include <chrono>
#include <algorithm>

namespace evp {

Q_DECLARE_LOGGING_CATEGORY(videoTiming)

// Each counter belongs to a single capture or encoder sequence. Enabling the
// category reports CPU wall time (including driver waits) in one-second windows.
class VideoStageTiming {
public:
    explicit VideoStageTiming(const char* stage) : stage_(stage) {}

    template<class Operation>
    auto measure(Operation&& operation) {
        if (!videoTiming().isDebugEnabled()) return operation();
        const auto start = Clock::now();
        auto result = operation();
        const auto end = Clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        if (!count_) window_ = start;
        ++count_;
        totalUs_ += us;
        maxUs_ = std::max<qint64>(maxUs_, us);
        if (end - window_ >= std::chrono::seconds(1)) {
            qCDebug(videoTiming) << stage_ << "samples" << count_ << "mean-us" << totalUs_ / count_
                                 << "max-us" << maxUs_;
            count_ = totalUs_ = maxUs_ = 0;
        }
        return result;
    }

private:
    using Clock = std::chrono::steady_clock;
    const char* stage_;
    Clock::time_point window_{};
    qint64 count_ = 0, totalUs_ = 0, maxUs_ = 0;
};

} // namespace evp
