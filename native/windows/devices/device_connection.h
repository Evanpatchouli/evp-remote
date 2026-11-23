#pragma once

#include "code_options.h"
#include "device_identity.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <optional>

class DevicePage;
class HomeWindow;
class QWebSocket;

namespace evp {

class DesktopController;

class DeviceConnection final : public QObject {
public:
    DeviceConnection(DesktopController& controller, HomeWindow& home, QString storePath, const QUrl& initialAddress);
    ~DeviceConnection() override;
    bool busy() const;
    QUrl signalingUrl() const;
    QString deploymentKey() const;
    void updateActivity();
    bool sendMedia(const QJsonObject& data);
    void sessionEnded(const QString& message, bool notify, bool declined);

private:
    enum class Registration { Offline, Connecting, Challenge, Registering, Online };
    enum class Stage { Challenge, Approval, Confirm, Paired, Draining, Ready, Active };

    struct Attempt {
        QString id;
        QString peer;
        QByteArray key;
        QString name;
        QString nonce;
        Stage stage = Stage::Challenge;
        qint64 sent = 0;
        qint64 received = 0;
        bool host = false;
        bool pair = false;
        bool relayOnly = false;
    };

    void configure(const QUrl& url, const QString& key, bool forgetKey);
    void openSocket();
    void closeSocket();
    void offline(const QString& message, bool retry);
    bool send(const QJsonObject& message);
    void receive(const QJsonObject& message);
    void pollPresence();
    void setOnlinePeers(QSet<QString> online);
    bool persist();
    void storageUnavailable(QString message);
    void changePreferences(DevicePreferences options);
    void renamePeer(const QString& id, const QString& name);
    void movePeer(const QString& id, DeviceMove move);
    void pinPeer(const QString& id, bool pinned);
    void savePeerOrder(QMap<QString, DevicePeer> peers, const QStringList& ids);
    void savePeerPresentation(QMap<QString, DevicePeer> peers);
    void removePeer(const QString& id);
    void makePairingCode(CodeOptions options);
    void clearPairingCode();
    void start(const QString& target, bool pair, const QByteArray& secret = {});
    void startPairing(const QString& code);
    void receiveRequest(const QJsonObject& message);
    void approvePairing(const QString& id, bool approve);
    void challenge();
    void receiveFrame(const QJsonObject& message);
    void peerChallenged(const QJsonObject& data);
    void peerConfirmed();
    void ready(const QJsonObject& message);
    bool rememberPeer();
    bool locallyAuthorized(const Attempt& attempt) const;
    QByteArray frameProof(const QString& nonce, bool fromHost, const QString& stage,
                          qint64 sequence, const QString& payload) const;
    bool sendFrame(const QString& stage, const QJsonObject& data);
    void failAttempt(const QString& message, bool notify = true, bool declined = false);
    void clearAttempt(bool notify, bool declined = false);

    DesktopController& controller_;
    HomeWindow& home_;
    DevicePage& page_;
    DeviceStore store_;
    QString id_;
    QString name_;
    QWebSocket* socket_ = nullptr;
    Registration registration_ = Registration::Offline;
    QTimer reconnect_, handshake_, presence_, attemptTimeout_, pairingExpiry_;
    QElapsedTimer presenceAge_, pairingAge_;
    int retryMs_ = 1000;
    QSet<QString> online_;
    QByteArray pairingSecret_;
    CodeOptions pairingOptions_;
    std::optional<Attempt> attempt_;
    bool storageFault_ = false;
    bool storeOpen_ = false;
};

}
