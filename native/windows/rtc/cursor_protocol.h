#pragma once

#include <QDataStream>
#include <QImage>
#include <QIODevice>

namespace evp {

inline constexpr char cursorChannelLabel[] = "cursor-v1";
inline constexpr int maxCursorEdge = 128;
inline constexpr int cursorHeaderBytes = 37;
inline constexpr int maxCursorPacket = 64 * 1024;
inline constexpr unsigned cursorWindow = 8;

enum class CursorMode : quint8 { Embedded, Hidden, Separate, Composited };

struct CursorSnapshot {
    CursorMode mode = CursorMode::Embedded;
    QSize desktop;
    QPoint topLeft;
    QPoint hotSpot;
    QImage image;
    quint32 appliedMove = 0;

    bool operator==(const CursorSnapshot& other) const {
        return mode == other.mode && desktop == other.desktop && topLeft == other.topLeft &&
            hotSpot == other.hotSpot && image.cacheKey() == other.image.cacheKey() &&
            appliedMove == other.appliedMove;
    }
};

struct CursorPacket {
    quint64 epoch = 0;
    quint32 sequence = 0;
    CursorSnapshot state;
    quint64 shapeId = 0;
    QSize shapeSize;
};

inline QByteArray encodeCursor(const CursorSnapshot& state, quint64 epoch, quint32 sequence, bool includeShape) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << epoch << sequence << quint8(state.mode) << quint16(state.desktop.width()) << quint16(state.desktop.height())
        << qint16(state.topLeft.x()) << qint16(state.topLeft.y())
        << quint8(state.image.isNull() ? 0 : state.image.width())
        << quint8(state.image.isNull() ? 0 : state.image.height())
        << quint8(state.hotSpot.x()) << quint8(state.hotSpot.y()) << state.appliedMove
        << quint64(state.image.cacheKey());
    if (includeShape && !state.image.isNull()) {
        for (int row = 0; row < state.image.height(); ++row)
            out.writeRawData(reinterpret_cast<const char*>(state.image.constScanLine(row)), 4 * state.image.width());
    }
    return bytes;
}

inline bool decodeCursor(const QByteArray& bytes, CursorPacket& packet) {
    if (bytes.size() < cursorHeaderBytes || bytes.size() > maxCursorPacket) return false;
    QDataStream in(bytes);
    quint8 mode, width, height, hotX, hotY;
    quint16 desktopWidth, desktopHeight;
    qint16 x, y;
    auto& state = packet.state;
    in >> packet.epoch >> packet.sequence >> mode >> desktopWidth >> desktopHeight
        >> x >> y >> width >> height >> hotX >> hotY >> state.appliedMove >> packet.shapeId;
    if (in.status() != QDataStream::Ok || packet.epoch == 0 || packet.sequence == 0 ||
        mode > quint8(CursorMode::Composited) || desktopWidth < 2 || desktopHeight < 2 ||
        desktopWidth > 16384 || desktopHeight > 16384) return false;

    state.mode = CursorMode(mode);
    state.desktop = QSize(desktopWidth, desktopHeight);
    state.topLeft = QPoint(x, y);
    state.hotSpot = QPoint(hotX, hotY);
    state.image = {};
    packet.shapeSize = QSize(width, height);
    const auto payload = bytes.size() - cursorHeaderBytes;
    if (state.mode != CursorMode::Separate) {
        return !payload && !packet.shapeId && !width && !height && !hotX && !hotY && !x && !y && !state.appliedMove;
    }
    if (!packet.shapeId || !width || !height || width > maxCursorEdge || height > maxCursorEdge ||
        cursorHeaderBytes + 4 * width * height > maxCursorPacket ||
        hotX >= width || hotY >= height || x + hotX < 0 || x + hotX >= desktopWidth ||
        y + hotY < 0 || y + hotY >= desktopHeight || (payload && payload != 4 * width * height)) return false;
    if (payload) {
        state.image = QImage(packet.shapeSize, QImage::Format_ARGB32_Premultiplied);
        if (state.image.isNull()) return false;
        for (int row = 0; row < height; ++row)
            in.readRawData(reinterpret_cast<char*>(state.image.scanLine(row)), 4 * width);
    }
    return in.status() == QDataStream::Ok;
}

inline QByteArray encodeCursorAck(quint32 sequence) {
    QByteArray bytes;
    QDataStream(&bytes, QIODevice::WriteOnly) << sequence;
    return bytes;
}

inline quint32 decodeCursorAck(const QByteArray& bytes) {
    if (bytes.size() != sizeof(quint32)) return 0;
    quint32 sequence = 0;
    QDataStream(bytes) >> sequence;
    return sequence;
}

} // namespace evp
