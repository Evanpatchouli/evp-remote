#pragma once

#include "rtc/cursor_protocol.h"

#include <QRectF>
#include <optional>
#include <utility>

namespace evp {

class ViewerCursor {
public:
    bool receive(const CursorPacket& packet) {
        if (packet.sequence <= sequence_) return false;
        const auto& state = packet.state;
        if (state.mode == CursorMode::Separate && state.image.isNull() &&
            (packet.shapeId != shapeId_ || packet.shapeSize != cursor_.image.size())) return false;
        const auto previousShape = std::move(cursor_.image);
        const auto previousDesktop = cursor_.desktop;
        cursor_ = state;
        sequence_ = packet.sequence;
        if (!state.image.isNull()) shapeId_ = packet.shapeId;
        else cursor_.image = previousShape;
        if (state.mode != CursorMode::Separate || previousDesktop != state.desktop ||
            state.appliedMove == moveSerial_) expirePrediction();
        return true;
    }

    bool hidesLocalPointer(bool inputAvailable, bool focused, bool insideContent) const {
        return sequence_ && inputAvailable && focused && insideContent;
    }

    bool separate() const { return sequence_ && cursor_.mode == CursorMode::Separate; }
    const QImage& image() const { return cursor_.image; }
    quint32 nextMoveSerial() const { return moveSerial_ == UINT32_MAX ? 1 : moveSerial_ + 1; }

    void predict(quint32 serial, QPointF position) {
        moveSerial_ = serial;
        prediction_ = position;
    }

    void expirePrediction() { prediction_.reset(); }

    QRectF rectangle(bool localActive) const {
        if (!separate()) return {};
        QPointF topLeft = cursor_.topLeft;
        const double width = cursor_.desktop.width();
        const double height = cursor_.desktop.height();
        if (localActive && prediction_) {
            topLeft = QPointF(prediction_->x() * (width - 1), prediction_->y() * (height - 1)) - cursor_.hotSpot;
        }
        return {topLeft.x() / width, topLeft.y() / height, cursor_.image.width() / width, cursor_.image.height() / height};
    }

private:
    CursorSnapshot cursor_;
    quint32 sequence_ = 0;
    quint64 shapeId_ = 0;
    quint32 moveSerial_ = 0;
    std::optional<QPointF> prediction_;
};

} // namespace evp
