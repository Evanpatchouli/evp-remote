#include "device_connection.h"
#include "controller.h"
#include "ui/device_page.h"
#include "ui/home_window.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QSysInfo>
#include <QWebSocket>
#include <algorithm>
#include <utility>

namespace evp {
namespace {

constexpr int presenceMs = 4000;
constexpr int pendingMs = 60000;
constexpr int sessionMs = 60 * 60 * 1000;
const auto secureFailure = QStringLiteral("设备身份或会话验证失败，已停止连接。可检查配对关系后重试。");
const auto storageFailure = QStringLiteral("本地设置尚未保存，可信设备远控已暂停。请恢复存储、核对配对和权限，再点击「保存并连接」。");

QString modeName(bool pair) {
    return pair ? QStringLiteral("pair") : QStringLiteral("control");
}

QString encodePayload(const QJsonObject& object) {
    return encodeDeviceBytes(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QByteArray pairingMac(const QByteArray& secret, const QString& viewer, const QString& host, const QString& attempt) {
    return QMessageAuthenticationCode::hash(deviceProof({"pair-code", viewer, host, attempt}), secret,
                                            QCryptographicHash::Sha256);
}

QString errorMessage(const QString& code) {
    if (code == "offline")
        return QStringLiteral("对方设备已离线，请确认客户端正在运行。");
    if (code == "busy")
        return QStringLiteral("对方设备正在使用其他会话，请稍后重试。");
    if (code == "relay")
        return QStringLiteral("目标设为仅中继，但当前信令服务器未配置 TURN。");
    if (code == "capacity")
        return QStringLiteral("信令服务器暂时达到容量上限，请稍后重试。");
    return QStringLiteral("信令服务器未能完成该设备请求。");
}

}

DeviceConnection::DeviceConnection(DesktopController& controller, HomeWindow& home, QString storePath)
    : controller_(controller), home_(home), page_(home.devices()), store_(std::move(storePath)),
      name_(deviceName(QSysInfo::machineHostName())) {
    reconnect_.setSingleShot(true);
    handshake_.setSingleShot(true);
    attemptTimeout_.setSingleShot(true);
    attemptTimeout_.setTimerType(Qt::PreciseTimer);
    pairingExpiry_.setSingleShot(true);
    pairingExpiry_.setTimerType(Qt::PreciseTimer);
    presence_.setInterval(presenceMs);
    connect(&reconnect_, &QTimer::timeout, this, &DeviceConnection::openSocket);
    connect(&handshake_, &QTimer::timeout, this, [this] {
        offline(QStringLiteral("连接设备信令超时，正在等待重试。"), true);
    });
    connect(&presence_, &QTimer::timeout, this, &DeviceConnection::pollPresence);
    connect(&attemptTimeout_, &QTimer::timeout, this, [this] {
        failAttempt(attempt_ && attempt_->stage == Stage::Active
            ? QStringLiteral("本次可信设备会话已达到一小时上限。需要继续时可重新连接。")
            : QStringLiteral("设备请求已超时，请重试。"));
    });
    connect(&pairingExpiry_, &QTimer::timeout, this, [this] {
        clearPairingCode();
        page_.setMessage(QStringLiteral("配对码已过期。需要配对时，请重新生成。"));
    });
    if (!store_.open() || (id_ = deviceId(store_.identity.publicKey())).isEmpty()) {
        page_.setRegistration(false, QStringLiteral("设备存储不可用，或已有另一个客户端在运行。仍可使用远程协助。"));
        return;
    }
    page_.setIdentity(id_);
    if (!store_.url.isEmpty()) {
        page_.setServer(store_.url);
        home_.showDevicePage();
    }
    page_.setPreferences(store_.preferences, true);
    page_.setDevices(store_.peers, {});
    connect(&page_, &DevicePage::configureRequested, this, &DeviceConnection::configure);
    connect(&page_, &DevicePage::codeRequested, this, &DeviceConnection::makePairingCode);
    connect(&page_, &DevicePage::pairingOptionsEdited, this, [this] {
        if (!pairingSecret_.isEmpty()) {
            clearPairingCode();
            page_.setMessage(QStringLiteral("配对选项已更改，原配对码已失效。请重新生成。"));
        } else if (attempt_ && attempt_->host && attempt_->pair &&
                   (attempt_->stage == Stage::Approval || attempt_->stage == Stage::Confirm)) {
            failAttempt(QStringLiteral("配对选项已更改，已取消此次配对。"), true, true);
        }
    });
    connect(&page_, &DevicePage::pairingRequested, this, &DeviceConnection::startPairing);
    connect(&page_, &DevicePage::pairingDecided, this, &DeviceConnection::approvePairing);
    connect(&page_, &DevicePage::connectRequested, this, [this](const QString& id) { start(id, false); });
    connect(&page_, &DevicePage::removeRequested, this, &DeviceConnection::removePeer);
    connect(&page_, &DevicePage::preferencesChanged, this,
            [this](bool incoming, bool audio, bool highRefresh, bool relayOnly) {
                changePreferences({incoming, audio, highRefresh, relayOnly});
            });
    connect(&page_, &DevicePage::cancelRequested, this, [this] {
        failAttempt(QStringLiteral("已取消设备请求。"));
    });
    if (store_.hasStorageHold()) {
        storageFault_ = true;
        store_.preferences.allowIncoming = false;
        page_.setPreferences(store_.preferences, true);
        page_.setRegistration(false, storageFailure);
        return;
    }
    if (store_.preferences.allowIncoming && !persist())
        return;
    if (!store_.url.isEmpty())
        reconnect_.start(0);
}

DeviceConnection::~DeviceConnection() {
    closeSocket();
}

bool DeviceConnection::busy() const {
    return attempt_.has_value();
}

void DeviceConnection::updateActivity() {
    const bool sessionBusy = controller_.phase_ != DesktopController::Phase::Idle;
    if (sessionBusy && !pairingSecret_.isEmpty())
        clearPairingCode();
    page_.setActivity(sessionBusy, busy());
}

void DeviceConnection::configure(const QUrl& input, const QString& key) {
    if (busy() || controller_.phase_ != DesktopController::Phase::Idle)
        return;
    const auto url = deviceServerUrl(input);
    const auto secret = (key.isEmpty() && url == store_.url) ? store_.deploymentKey : key;
    if (url.isEmpty() || url.toEncoded().size() > 2048) {
        page_.setMessage(QStringLiteral("请输入有效的 wss:// 信令地址。"));
        return;
    }
    if (secret.trimmed().isEmpty() || secret.toUtf8().size() > 4096) {
        page_.setMessage(QStringLiteral("请输入此信令服务器的部署密钥。"));
        return;
    }
    store_.url = url;
    store_.deploymentKey = secret;
    if (!persist())
        return;
    page_.setServer(url);
    page_.setMessage({});
    retryMs_ = 1000;
    reconnect_.stop();
    closeSocket();
    clearPairingCode();
    openSocket();
}

void DeviceConnection::openSocket() {
    if (socket_ || store_.url.isEmpty() || storageFault_)
        return;
    registration_ = Registration::Connecting;
    page_.setRegistration(false, QStringLiteral("正在连接设备信令服务…"));
    socket_ = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
    socket_->setMaxAllowedIncomingMessageSize(65536);
    socket_->setMaxAllowedIncomingFrameSize(65536);
    connect(socket_, &QWebSocket::connected, this, [this] {
        registration_ = Registration::Challenge;
        if (!send({{"type", "device-hello"}, {"key", store_.deploymentKey}}))
            offline(QStringLiteral("无法注册本机设备，正在等待重试。"), true);
    });
    connect(socket_, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
        const auto document = QJsonDocument::fromJson(text.toUtf8());
        if (document.isObject())
            receive(document.object());
        else
            offline(QStringLiteral("信令服务返回了无效的设备消息。"), false);
    });
    connect(socket_, &QWebSocket::binaryMessageReceived, this, [this] {
        offline(QStringLiteral("信令服务返回了无效的设备消息。"), false);
    });
    connect(socket_, &QWebSocket::disconnected, this, [this] {
        offline(QStringLiteral("设备信令已断开，正在等待重连。"), true);
    });
    connect(socket_, &QWebSocket::sslErrors, this, [this] {
        offline(QStringLiteral("信令服务器的 TLS 证书不受信任。请核对地址及证书后重试。"), false);
    });
    connect(socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        offline(QStringLiteral("设备信令连接失败，正在等待重试。"), true);
    });
    handshake_.start(12000);
    socket_->open(store_.url);
}

void DeviceConnection::closeSocket() {
    registration_ = Registration::Offline;
    handshake_.stop();
    presence_.stop();
    presenceAge_.invalidate();
    if (auto* socket = std::exchange(socket_, nullptr)) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
}

void DeviceConnection::offline(const QString& message, bool retry) {
    closeSocket();
    reconnect_.stop();
    if (attempt_)
        failAttempt(message, false);
    clearPairingCode();
    setOnlinePeers({});
    page_.setRegistration(false, message);
    if (retry && !storageFault_) {
        reconnect_.start(retryMs_);
        retryMs_ = std::min(retryMs_ * 2, 30000);
    }
}

bool DeviceConnection::send(const QJsonObject& message) {
    return socket_ && socket_->state() == QAbstractSocket::ConnectedState &&
           socket_->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact))) > 0;
}

void DeviceConnection::receive(const QJsonObject& message) {
    const auto type = message.value("type").toString();
    if (registration_ != Registration::Online) {
        if (registration_ == Registration::Challenge && type == "device-challenge") {
            const auto nonce = message.value("nonce").toString();
            const auto publicKey = encodeDeviceBytes(store_.identity.publicKey());
            const auto signature = decodeDeviceBytes(nonce, 32).isEmpty() ? QString{} :
                store_.identity.sign(deviceProof({"register", nonce, publicKey}));
            if (!signature.isEmpty()) {
                registration_ = Registration::Registering;
                if (send({{"type", "device-register"}, {"publicKey", publicKey}, {"signature", signature}}))
                    return;
            }
        } else if (registration_ == Registration::Registering && type == "device-registered" &&
                   message.value("id") == id_) {
            registration_ = Registration::Online;
            handshake_.stop();
            retryMs_ = 1000;
            page_.setRegistration(true, QStringLiteral("本机在线 · 无需登录"));
            presenceAge_.start();
            presence_.start();
            pollPresence();
            return;
        } else if (type == "device-error") {
            const auto code = message.value("code").toString();
            if (code == "auth") {
                offline(QStringLiteral("部署密钥或设备注册凭证无效，请核对后重新保存。"), false);
                return;
            }
            if (code == "capacity" || code == "duplicate") {
                offline(code == "duplicate" ? QStringLiteral("本机 ID 已在线，请关闭其他客户端或等待服务器重试。")
                                             : errorMessage(code), true);
                return;
            }
        }
        offline(QStringLiteral("信令服务不支持当前设备协议，或注册验证失败。请确认服务端已升级。"), false);
        return;
    }

    if (type == "device-presence") {
        const auto entries = message.value("online");
        if (!entries.isArray() || entries.toArray().size() > maxDevicePeers)
            return;
        QSet<QString> online;
        for (const auto& entry : entries.toArray()) {
            const auto id = entry.toString();
            if (store_.peers.contains(id))
                online.insert(id);
        }
        presenceAge_.restart();
        setOnlinePeers(std::move(online));
    } else if (type == "device-request") {
        receiveRequest(message);
    } else if (attempt_ && message.value("attempt") == attempt_->id) {
        if (type == "device-frame")
            receiveFrame(message);
        else if (type == "device-ready")
            ready(message);
        else if (type == "device-error")
            failAttempt(errorMessage(message.value("code").toString()));
        else if (type == "device-ended") {
            const auto reason = message.value("reason");
            if (attempt_->stage == Stage::Draining) {
                clearAttempt(false);
                page_.setMessage(reason == "ended" ? QStringLiteral("本机已保存配对，可在设备列表查看。") :
                    QStringLiteral("本机已保存配对，但未确认对方也已保存。若不能连接，请重新配对。"));
                pollPresence();
            } else {
                failAttempt(reason == "disconnected" ? QStringLiteral("对方设备已离线，连接已结束。") :
                    reason == "timeout" ? QStringLiteral("设备请求或会话已超时。") :
                    reason == "declined" ? QStringLiteral("对方未授权或已拒绝。请检查目标的配对、远控开关及桌面状态。") :
                    QStringLiteral("对方已结束设备连接。"), false);
            }
        }
    }
}

void DeviceConnection::pollPresence() {
    if (registration_ != Registration::Online)
        return;
    if (presenceAge_.hasExpired(15000)) {
        offline(QStringLiteral("设备信令未响应，正在等待重连。"), true);
        return;
    }
    QJsonArray ids;
    for (auto it = store_.peers.cbegin(); it != store_.peers.cend(); ++it)
        ids.append(it.key());
    if (!send({{"type", "device-presence"}, {"ids", ids}}))
        offline(QStringLiteral("设备信令已断开，正在等待重连。"), true);
}

void DeviceConnection::setOnlinePeers(QSet<QString> online) {
    if (online_ == online)
        return;
    online_ = std::move(online);
    page_.setDevices(store_.peers, online_);
}

bool DeviceConnection::persist() {
    if (store_.save()) {
        storageFault_ = false;
        return true;
    }
    storageFault_ = true;
    auto message = storageFailure;
    if (!store_.markStorageHold())
        message += QStringLiteral(" 无法记录停用状态，重启后可能恢复旧授权。");
    store_.preferences.allowIncoming = false;
    page_.setPreferences(store_.preferences, true);
    offline(message, false);
    page_.setMessage(message);
    return false;
}

void DeviceConnection::changePreferences(DevicePreferences options) {
    if (busy() || controller_.phase_ != DesktopController::Phase::Idle) {
        const bool revoke = store_.preferences.allowIncoming && !options.allowIncoming;
        options = store_.preferences;
        if (revoke)
            options.allowIncoming = false;
        page_.setPreferences(options, true);
        if (!revoke)
            return;
    }
    store_.preferences = options;
    if (!options.allowIncoming && attempt_ && attempt_->host && !attempt_->pair)
        failAttempt(QStringLiteral("已关闭可信设备远控，本机共享已停止。"));
    const bool recovering = storageFault_;
    if (persist() && recovering) {
        page_.setMessage({});
        if (!store_.url.isEmpty())
            reconnect_.start(0);
        else
            page_.setRegistration(false, QStringLiteral("配置信令服务器后即可添加设备。"));
    }
}

void DeviceConnection::removePeer(const QString& id) {
    if (!store_.peers.remove(id))
        return;
    if (attempt_ && attempt_->peer == id)
        failAttempt(QStringLiteral("已在本机撤销配对，连接已停止。"));
    online_.remove(id);
    page_.setDevices(store_.peers, online_);
    if (persist()) {
        page_.setMessage(QStringLiteral("已在本机移除配对。再次连接前需要重新配对。"));
        pollPresence();
    }
}

void DeviceConnection::makePairingCode(CodeOptions options) {
    if (registration_ != Registration::Online || busy() || controller_.phase_ != DesktopController::Phase::Idle)
        return;
    clearPairingCode();
    if (!isCodeLifetime(options.minutes)) {
        page_.setMessage(QStringLiteral("请选择有效的配对码期限。"));
        return;
    }
    pairingSecret_ = deviceRandom();
    if (pairingSecret_.isEmpty()) {
        page_.setMessage(QStringLiteral("无法生成安全配对码。"));
        return;
    }
    pairingOptions_ = options;
    pairingAge_.start();
    pairingExpiry_.start(options.minutes * 60 * 1000);
    page_.setPairingCode(QStringLiteral("EVP1.%1.%2").arg(id_, encodeDeviceBytes(pairingSecret_)), options);
    page_.setMessage(QStringLiteral("仅向自己的设备发送配对码，等待另一台电脑请求。"));
}

void DeviceConnection::clearPairingCode() {
    pairingExpiry_.stop();
    pairingAge_.invalidate();
    pairingSecret_.clear();
    pairingOptions_ = {};
    page_.setPairingCode({});
}

void DeviceConnection::startPairing(const QString& code) {
    const auto fields = code.split('.');
    if (fields.size() == 3 && fields[0] == "EVP1" && !decodeDeviceBytes(fields[1], 32).isEmpty()) {
        const auto secret = decodeDeviceBytes(fields[2], 32);
        if (!secret.isEmpty()) {
            start(fields[1], true, secret);
            return;
        }
    }
    page_.setMessage(QStringLiteral("配对码格式不正确，请完整复制对方生成的配对码。"));
}

void DeviceConnection::start(const QString& target, bool pair, const QByteArray& secret) {
    if (registration_ != Registration::Online || busy() || controller_.phase_ != DesktopController::Phase::Idle)
        return;
    if (target == id_) {
        page_.setMessage(QStringLiteral("不能与本机配对。请在另一台设备上输入此配对码。"));
        return;
    }
    const auto known = store_.peers.constFind(target);
    if (pair && known == store_.peers.cend() && store_.peers.size() >= maxDevicePeers) {
        page_.setMessage(QStringLiteral("可信设备已达上限，请先移除不再使用的设备。"));
        return;
    }
    if (!pair && (known == store_.peers.cend() || !online_.contains(target))) {
        page_.setMessage(QStringLiteral("所选设备未配对或已离线。"));
        return;
    }
    const auto attemptId = encodeDeviceBytes(deviceRandom());
    if (attemptId.isEmpty()) {
        page_.setMessage(secureFailure);
        return;
    }
    const auto mode = modeName(pair);
    const auto payload = encodePayload(pair ? QJsonObject{{"mac", encodeDeviceBytes(pairingMac(secret, id_, target, attemptId))},
                                                          {"name", name_}} : QJsonObject{});
    const auto signature = store_.identity.sign(deviceProof({"start", mode, id_, target, attemptId, payload}));
    if (signature.isEmpty()) {
        page_.setMessage(secureFailure);
        return;
    }
    clearPairingCode();
    attempt_.emplace();
    attempt_->id = attemptId;
    attempt_->peer = target;
    attempt_->pair = pair;
    if (known != store_.peers.cend()) {
        attempt_->key = known->publicKey;
        attempt_->name = known->name;
    }
    attemptTimeout_.start(pendingMs);
    updateActivity();
    page_.setMessage(pair ? QStringLiteral("等待对方确认配对…") : QStringLiteral("正在验证设备并建立连接…"));
    if (!send({{"type", "device-start"}, {"mode", mode}, {"target", target}, {"attempt", attemptId},
               {"payload", payload}, {"signature", signature}}))
        failAttempt(QStringLiteral("无法向信令服务发送设备请求。"), false);
}

void DeviceConnection::receiveRequest(const QJsonObject& message) {
    const auto attemptId = message.value("attempt").toString();
    if (decodeDeviceBytes(attemptId, 32).isEmpty() || (attempt_ && attempt_->id == attemptId))
        return;
    const auto decline = [this, &attemptId] {
        send({{"type", "device-end"}, {"attempt", attemptId}, {"reason", "declined"}});
    };
    if (busy() || controller_.phase_ != DesktopController::Phase::Idle || storageFault_) {
        decline();
        return;
    }
    const auto mode = message.value("mode").toString();
    const auto peer = message.value("from").toString();
    const auto key = decodeDeviceBytes(message.value("publicKey").toString(), 64);
    const auto encoded = message.value("payload").toString();
    const auto document = QJsonDocument::fromJson(decodeDeviceBytes(encoded));
    if ((mode != "pair" && mode != "control") || peer.isEmpty() || peer == id_ || deviceId(key) != peer ||
        !document.isObject()) {
        decline();
        return;
    }
    const auto known = store_.peers.constFind(peer);
    const bool pair = mode == "pair";
    const auto data = document.object();
    if (pair) {
        if ((known == store_.peers.cend() && store_.peers.size() >= maxDevicePeers) ||
            pairingSecret_.isEmpty() || !pairingAge_.isValid() ||
            !equalDeviceSecrets(decodeDeviceBytes(data.value("mac").toString(), 32),
                                pairingMac(pairingSecret_, peer, id_, attemptId))) {
            decline();
            return;
        }
    } else if (!store_.preferences.allowIncoming || known == store_.peers.cend() || known->publicKey != key ||
               !data.isEmpty()) {
        decline();
        return;
    }
    const auto proof = deviceProof({"start", mode, peer, id_, attemptId, encoded});
    if (!DeviceIdentity::verify(key, proof, message.value("signature").toString()) ||
        (pair && pairingAge_.elapsed() >= pairingExpiry_.interval())) {
        decline();
        return;
    }
    const bool preapproved = pair && pairingOptions_.autoApprove;
    clearPairingCode();
    attempt_.emplace();
    attempt_->id = attemptId;
    attempt_->peer = peer;
    attempt_->key = key;
    attempt_->name = pair ? deviceName(data.value("name").toString()) : known->name;
    attempt_->pair = pair;
    attempt_->host = true;
    attempt_->relayOnly = store_.preferences.relayOnly;
    attemptTimeout_.start(pendingMs);
    updateActivity();
    if (pair) {
        attempt_->stage = Stage::Approval;
        if (preapproved) {
            approvePairing(attemptId, true);
            return;
        }
        page_.setPairingRequest(attemptId, attempt_->name, peer);
        page_.setMessage({});
        home_.showDevicePage();
        QApplication::alert(&home_);
    } else {
        page_.setMessage(QStringLiteral("正在验证可信设备「%1」…").arg(attempt_->name));
        challenge();
    }
}

void DeviceConnection::approvePairing(const QString& id, bool approve) {
    if (!attempt_ || attempt_->id != id || !attempt_->pair || !attempt_->host || attempt_->stage != Stage::Approval)
        return;
    page_.setPairingRequest({}, {}, {});
    if (!approve) {
        failAttempt(QStringLiteral("已拒绝配对请求。"), true, true);
        return;
    }
    page_.setMessage(QStringLiteral("正在验证并保存本地配对…"));
    challenge();
}

void DeviceConnection::challenge() {
    attempt_->nonce = encodeDeviceBytes(deviceRandom());
    if (attempt_->nonce.isEmpty()) {
        failAttempt(secureFailure, true, true);
        return;
    }
    QJsonObject data{{"nonce", attempt_->nonce}};
    if (attempt_->pair) {
        data.insert("publicKey", encodeDeviceBytes(store_.identity.publicKey()));
        data.insert("name", name_);
    } else {
        data.insert("relayOnly", attempt_->relayOnly);
    }
    attempt_->stage = Stage::Confirm;
    if (!sendFrame(QStringLiteral("challenge"), data))
        failAttempt(secureFailure, true, true);
}

QByteArray DeviceConnection::frameProof(const QString& nonce, bool fromHost, const QString& stage,
                                        qint64 sequence, const QString& payload) const {
    const auto& current = *attempt_;
    return deviceProof({"frame", modeName(current.pair), current.host ? current.peer : id_,
                        current.host ? id_ : current.peer, current.id, nonce, fromHost ? "host" : "viewer",
                        stage, QString::number(sequence), payload});
}

bool DeviceConnection::sendFrame(const QString& stage, const QJsonObject& data) {
    const auto payload = encodePayload(data);
    if (payload.size() > maxDevicePayload)
        return false;
    const auto sequence = attempt_->sent + 1;
    const auto proof = frameProof(attempt_->nonce, attempt_->host, stage, sequence, payload);
    const auto signature = store_.identity.sign(proof);
    if (signature.isEmpty())
        return false;
    attempt_->sent = sequence;
    return send({{"type", "device-frame"}, {"attempt", attempt_->id}, {"stage", stage}, {"seq", sequence},
                 {"payload", payload}, {"signature", signature}});
}

void DeviceConnection::receiveFrame(const QJsonObject& message) {
    const auto stage = message.value("stage").toString();
    const auto sequence = message.value("seq").toInteger(-1);
    const bool initial = !attempt_->host && attempt_->stage == Stage::Challenge && stage == "challenge";
    const bool confirm = attempt_->host && attempt_->stage == Stage::Confirm && stage == "confirm";
    const bool paired = !attempt_->host && attempt_->pair && attempt_->stage == Stage::Paired && stage == "paired";
    const bool media = !attempt_->pair && attempt_->stage == Stage::Active && stage == "signal";
    if ((!initial && !confirm && !paired && !media) || sequence != attempt_->received + 1) {
        failAttempt(secureFailure, true, true);
        return;
    }
    const auto encoded = message.value("payload").toString();
    const auto document = QJsonDocument::fromJson(decodeDeviceBytes(encoded));
    const auto data = document.object();
    const auto nonce = initial ? data.value("nonce").toString() : attempt_->nonce;
    const auto key = initial && attempt_->pair ? decodeDeviceBytes(data.value("publicKey").toString(), 64)
                                                : attempt_->key;
    if (!document.isObject() || ((confirm || paired) && !data.isEmpty()) ||
        (initial && decodeDeviceBytes(nonce, 32).isEmpty()) || deviceId(key) != attempt_->peer ||
        !DeviceIdentity::verify(key, frameProof(nonce, !attempt_->host, stage, sequence, encoded),
                                message.value("signature").toString())) {
        failAttempt(secureFailure, true, true);
        return;
    }
    attempt_->received = sequence;
    if (initial) {
        attempt_->nonce = nonce;
        attempt_->key = key;
        peerChallenged(data);
    } else if (confirm) {
        peerConfirmed();
    } else if (paired) {
        if (rememberPeer()) {
            clearAttempt(true);
            page_.setMessage(QStringLiteral("已配对。以后可从设备列表直接连接。"));
            pollPresence();
        }
    } else if (locallyAuthorized(*attempt_)) {
        controller_.received({{"type", "signal"}, {"data", data}});
    } else {
        failAttempt(secureFailure, true, true);
    }
}

void DeviceConnection::peerChallenged(const QJsonObject& data) {
    if (attempt_->pair) {
        attempt_->name = deviceName(data.value("name").toString());
        attempt_->stage = Stage::Paired;
    } else {
        if (!data.value("relayOnly").isBool() || !locallyAuthorized(*attempt_)) {
            failAttempt(secureFailure, true, true);
            return;
        }
        attempt_->relayOnly = data.value("relayOnly").toBool();
        attempt_->stage = Stage::Ready;
    }
    if (!sendFrame(QStringLiteral("confirm"), {}))
        failAttempt(secureFailure, true, true);
}

void DeviceConnection::peerConfirmed() {
    if (attempt_->pair) {
        if (!rememberPeer())
            return;
        attempt_->stage = Stage::Draining;
        if (sendFrame(QStringLiteral("paired"), {}))
            page_.setMessage(QStringLiteral("本机已保存配对，等待双方完成。"));
        else
            failAttempt(QStringLiteral("本机已保存配对，但未能通知对方。请重新配对。"));
        return;
    }
    if (!locallyAuthorized(*attempt_) || !controller_.reserveDeviceSession(true, attempt_->relayOnly, store_.preferences)) {
        failAttempt(QStringLiteral("未授权，或当前桌面不支持远程控制。"), true, true);
        return;
    }
    attempt_->stage = Stage::Ready;
    updateActivity();
    if (!send({{"type", "device-accept"}, {"attempt", attempt_->id}, {"relayOnly", attempt_->relayOnly}}))
        failAttempt(QStringLiteral("无法建立共享连接。"));
}

void DeviceConnection::ready(const QJsonObject& message) {
    if (attempt_->pair || attempt_->stage != Stage::Ready || !locallyAuthorized(*attempt_) ||
        message.value("role") != (attempt_->host ? "host" : "viewer") || !message.value("relayOnly").isBool() ||
        message.value("relayOnly").toBool() != attempt_->relayOnly || !message.value("iceServers").isArray()) {
        failAttempt(secureFailure, true, true);
        return;
    }
    if (attempt_->host ? !controller_.trustedSession_ || controller_.phase_ != DesktopController::Phase::Approved
                       : !controller_.reserveDeviceSession(false, attempt_->relayOnly, store_.preferences)) {
        failAttempt(QStringLiteral("设备正在使用其他会话。"), true, true);
        return;
    }
    attempt_->stage = Stage::Active;
    attemptTimeout_.start(sessionMs);
    updateActivity();
    page_.setMessage(attempt_->host ? QStringLiteral("向可信设备「%1」共享本机。").arg(attempt_->name)
                                    : QStringLiteral("正在连接可信设备「%1」。").arg(attempt_->name));
    if (attempt_->host)
        QApplication::alert(&home_);
    controller_.ready(message);
}

bool DeviceConnection::rememberPeer() {
    const auto peer = attempt_->peer;
    const auto previous = store_.peers;
    store_.peers.insert(peer, {attempt_->key, attempt_->name});
    if (!persist()) {
        store_.peers = previous;
        page_.setDevices(store_.peers, online_);
        return false;
    }
    page_.setDevices(store_.peers, online_);
    return true;
}

bool DeviceConnection::locallyAuthorized(const Attempt& attempt) const {
    const auto trusted = store_.peers.constFind(attempt.peer);
    return !storageFault_ && trusted != store_.peers.cend() && trusted->publicKey == attempt.key &&
           (!attempt.host || store_.preferences.allowIncoming);
}

bool DeviceConnection::sendMedia(const QJsonObject& data) {
    return attempt_ && !attempt_->pair && attempt_->stage == Stage::Active && locallyAuthorized(*attempt_) &&
           sendFrame(QStringLiteral("signal"), data);
}

void DeviceConnection::clearAttempt(bool notify, bool declined) {
    const auto previous = std::exchange(attempt_, std::nullopt);
    attemptTimeout_.stop();
    page_.setPairingRequest({}, {}, {});
    if (notify && previous)
        send({{"type", "device-end"}, {"attempt", previous->id}, {"reason", declined ? "declined" : "ended"}});
    updateActivity();
}

void DeviceConnection::failAttempt(const QString& message, bool notify, bool declined) {
    if (!attempt_)
        return;
    if (controller_.trustedSession_) {
        controller_.finish(message, true, notify, declined ? QStringLiteral("declined") : QString{});
    } else {
        clearAttempt(notify, declined);
        page_.setMessage(message);
    }
}

void DeviceConnection::sessionEnded(const QString& message, bool notify, bool declined) {
    clearAttempt(notify, declined);
    page_.setMessage(message);
}

}
