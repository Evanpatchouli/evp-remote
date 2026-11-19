#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QSize>
#include <QObject>
#include <QTimer>
#include <memory>
#include "rtc/rtc_viewer.h"

class HomeWindow;
class SessionWindow;
class QWebSocket;
namespace evp {
class ViewerInput;
class VideoSurface;

class DesktopController final : public QObject {
    Q_OBJECT
  public:
    explicit DesktopController(HomeWindow& home);
    ~DesktopController() override;
    void endSession();
  signals:
    void sample(quint64 presents, QSize sourceSize, QString route, bool receivedAudio, bool inputReady,
                quint64 gpuDecoded, quint64 cpuDecoded);
    void sessionFinished(QString message);

  private:
    struct Dispatch;
    enum class Phase { Idle, Connecting, Joining, Waiting, Negotiating, Streaming };
    void join(const QUrl& url, const QString& invite);
    void received(const QJsonObject& message);
    void ready(const QJsonObject& message);
    void stateChanged(const std::string& state);
    void updateStats();
    void updatePermit();
    bool sendSignal(QJsonObject message);
    bool sendInput(QJsonObject event);
    void finish(const QString& message, bool failure, bool notify = true, QString reason = {});
    HomeWindow& home_;
    Phase phase_ = Phase::Idle;
    QWebSocket* socket_ = nullptr;
    std::unique_ptr<RtcViewer> rtc_;
    SessionWindow* window_ = nullptr;
    VideoSurface* surface_ = nullptr;
    ViewerInput* input_ = nullptr;
    std::shared_ptr<Dispatch> dispatch_;
    QTimer deadline_, sampler_;
    QElapsedTimer interval_;
    quint64 generation_ = 0, presented_ = 0, epoch_ = 0;
    bool connected_ = false, channel_ = false, audio_ = false, offered_ = false, relayOnly_ = false;
    bool everOpened_ = false, playout_ = false, inputFault_ = false;
    QString route_;
};
} // namespace evp
