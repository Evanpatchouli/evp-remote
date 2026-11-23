#pragma once

#include "viewer_cursor.h"

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

namespace evp {
class VideoSurface;

class ViewerInput final : public QObject {
  public:
    using Sender = std::function<bool(QJsonObject)>;
    ViewerInput(VideoSurface* surface, Sender sender, QObject* parent);
    ~ViewerInput() override;
    void setAvailable(bool available);
    void release();
    void receiveCursor(const CursorPacket& packet);
    void resetCursor();

  protected:
    bool eventFilter(QObject* object, QEvent* event) override;

  private:
    void updateCursor();
    VideoSurface* surface_;
    Sender send_;
    bool available_ = false;
    QSet<QString> keys_;
    QSet<int> buttons_;
    ViewerCursor cursor_;
    QTimer optimisticCursor_;
    bool localCursorHidden_ = false;
};
} // namespace evp
