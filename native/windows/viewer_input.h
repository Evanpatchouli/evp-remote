#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>
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

  protected:
    bool eventFilter(QObject* object, QEvent* event) override;

  private:
    VideoSurface* surface_;
    Sender send_;
    bool available_ = false;
    QSet<QString> keys_;
    QSet<int> buttons_;
};
} // namespace evp
