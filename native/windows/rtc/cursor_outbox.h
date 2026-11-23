#pragma once

#include "cursor_protocol.h"

#include <optional>
#include <utility>

namespace evp {

// Access is serialized by the peer's cursor mutex. The capture producer keeps
// the latest observation, and the transport keeps a small acknowledged window.
class CursorOutbox {
public:
    struct Update {
        quint64 epoch;
        CursorSnapshot state;
        quint32 sequence = 0;
        bool includeShape = false;
    };

    void observe(quint64 epoch, CursorSnapshot state) {
        latest_ = Update{epoch, std::move(state)};
    }

    void reset() {
        latest_.reset();
        sentShape_ = 0;
    }

    bool acknowledge(quint32 sequence) {
        if (sequence <= acked_ || sequence > sent_) return false;
        acked_ = sequence;
        return true;
    }

    quint32 acknowledged() const { return acked_; }
    bool inFlight() const { return acked_ != sent_; }

    bool schedule() {
        if (posted_ || !canSend()) return false;
        posted_ = true;
        return true;
    }

    std::optional<Update> take(quint64 epoch) {
        posted_ = false;
        if (!epoch || !canSend() || latest_->epoch != epoch) return {};
        auto next = std::exchange(latest_, std::nullopt);
        next->sequence = ++sent_;
        const auto shape = next->state.image.cacheKey();
        if (shape && shape != sentShape_) {
            next->includeShape = true;
            sentShape_ = shape;
            sentShapeAt_ = sent_;
        }
        return next;
    }

private:
    bool includesShape() const {
        const auto shape = latest_ ? latest_->state.image.cacheKey() : 0;
        return shape && shape != sentShape_;
    }

    bool canSend() const {
        return latest_ && sent_ - acked_ < cursorWindow && (!includesShape() || sentShapeAt_ <= acked_);
    }

    std::optional<Update> latest_;
    quint32 acked_ = 0;
    quint32 sent_ = 0;
    quint32 sentShapeAt_ = 0;
    qint64 sentShape_ = 0;
    bool posted_ = false;
};

} // namespace evp
