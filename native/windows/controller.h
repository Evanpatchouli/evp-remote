#pragma once

#include "code_options.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSize>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <memory>
#include "rtc/rtc_viewer.h"
#include <functional>

class HomeWindow;
class SessionWindow;
class QWebSocket;
namespace evp {
class ViewerInput;
class VideoSurface;
class HostInput;
class HostMediaSource;
class RtcHost;
class DeviceConnection;
struct DevicePreferences;

struct HostAdapters {
    std::function<std::unique_ptr<HostMediaSource>()> media;
    std::function<std::unique_ptr<HostInput>()> input;
};

class DesktopController final : public QObject {
    Q_OBJECT
  public:
    explicit DesktopController(HomeWindow& home, HostAdapters adapters = {}, QString deviceStorePath = {},
                               const QUrl& initialAddress = {});
    ~DesktopController() override;
    void endSession();
  signals:
    void sample(quint64 presents, QSize sourceSize, QString route, bool receivedAudio, bool inputReady,
                quint64 gpuDecoded, quint64 cpuDecoded);
    void sessionFinished(QString message);

  private:
    friend class DeviceConnection;
    struct Dispatch;
    enum class Role { Viewer, Host };
    enum class Phase {
        Idle,
        Connecting,
        Joining,
        Waiting,
        Inviting,
        PendingApproval,
        Approved,
        Negotiating,
        Streaming
    };
    void join(const QString& invite);
    void openSignaling(Role role, const QString& invite, CodeOptions options = {},
                       bool highRefresh = false, bool relayOnly = false, bool withAudio = true);
    void invitationIssued(const QJsonObject& message);
    void invitationRedeemed(const QJsonObject& message);
    bool reserveDeviceSession(bool host, bool relayOnly, const DevicePreferences& preferences);
    std::unique_ptr<HostInput> makeHostInput() const;
    void received(const QJsonObject& message);
    void ready(const QJsonObject& message);
    void hostApproved(quint64 requestId, bool highRefresh, bool relayOnly, bool withAudio);
    void hostDenied(quint64 requestId);
    void hostReady(const std::vector<IceServer>& servers);
    void hostStateChanged(const std::string& state);
    void hostChannelChanged(bool open);
    void grantHostInput();
    bool revokeHostInput();
    void retryInputRelease();
    void applyHostInput(quint64 grant, const QByteArray& json);
    void tickHost();
    void updateHostStatus();
    void forwardCandidate(IceCandidate candidate);
    void stateChanged(const std::string& state);
    void updateStats();
    void updatePermit();
    bool sendSignal(QJsonObject message);
    bool sendInput(QJsonObject event);
    void finish(const QString& message, bool failure, bool notify = true, QString reason = {});
    HomeWindow& home_;
    HostAdapters hostAdapters_;
    Role role_ = Role::Viewer;
    Phase phase_ = Phase::Idle;
    CodeOptions invitationOptions_;
    QByteArray invitationSecret_;
    QElapsedTimer invitationAge_;
    QWebSocket* socket_ = nullptr;
    std::unique_ptr<RtcViewer> rtc_;
    std::unique_ptr<RtcHost> host_;
    std::unique_ptr<HostInput> localInput_;
    std::unique_ptr<HostInput> releasingInput_;
    SessionWindow* window_ = nullptr;
    VideoSurface* surface_ = nullptr;
    ViewerInput* input_ = nullptr;
    std::shared_ptr<Dispatch> dispatch_;
    QTimer deadline_, sampler_, releaseRetry_;
    QElapsedTimer interval_;
    quint64 generation_ = 0, presented_ = 0, epoch_ = 0;
    bool connected_ = false, channel_ = false, audio_ = false, offered_ = false, relayOnly_ = false;
    bool everOpened_ = false, playout_ = false, inputFault_ = false;
    bool hostArmed_ = false, highRefresh_ = false, withAudio_ = true;
    bool trustedSession_ = false;
    QString route_, encoder_;
    QSize captureSize_;
    std::unique_ptr<DeviceConnection> devices_;
};
} // namespace evp
