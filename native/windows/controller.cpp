#include "controller.h"
#include "controller_dispatch.h"
#include "devices/device_connection.h"
#include "devices/device_identity.h"
#include "host_input.h"
#include "rtc/rtc_host.h"
#include "media/video_surface.h"
#include "media/video_timing.h"
#include "media/ffmpeg_decoder.h"
#include "ui/home_window.h"
#include "ui/session_window.h"
#include "viewer_input.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QUrl>
#include <QWebSocket>
#include <mutex>
#include <atomic>
#include <utility>
#include <d3d11.h>
#include <wrl/client.h>

namespace evp {
namespace {
QString invitationVerifier(const QByteArray& secret) {
    const auto encoded = encodeDeviceBytes(secret).toLatin1();
    return encodeDeviceBytes(QCryptographicHash::hash(encoded, QCryptographicHash::Sha256));
}

QString peerMessage(const QString& raw) {
    if (raw.contains("Relay-only mode requires", Qt::CaseInsensitive))
        return QStringLiteral("信令服务未配置 TURN。请配置中继，或重新邀请并取消「仅使用 TURN 中继」。");
    if (raw.contains("access key", Qt::CaseInsensitive))
        return QStringLiteral("部署访问密钥不正确，请核对后重试。");
    if (raw.contains("invalid, expired", Qt::CaseInsensitive))
        return QStringLiteral("邀请码无效、已过期或已使用。请对方重新创建邀请。");
    if (raw.contains("declined", Qt::CaseInsensitive))
        return QStringLiteral("对方已拒绝本次远程协助。");
    if (raw.contains("audio", Qt::CaseInsensitive))
        return QStringLiteral("系统声音不可用。请对方共享整个屏幕并勾选共享系统音频。");
    if (raw.contains("screen", Qt::CaseInsensitive) || raw.contains("display", Qt::CaseInsensitive))
        return QStringLiteral("对方未能共享屏幕。请确认显示器状态并重新邀请。");
    if (raw.contains("input", Qt::CaseInsensitive))
        return QStringLiteral("对方的键鼠适配器不可用。请重新打开被控端。");
    if (raw.contains("expired", Qt::CaseInsensitive))
        return QStringLiteral("会话或授权请求已过期。请重新邀请。");
    if (raw.contains("media connection", Qt::CaseInsensitive))
        return QStringLiteral("媒体连接中断，请检查两端网络和中继。");
    if (raw.contains("disconnect", Qt::CaseInsensitive))
        return QStringLiteral("对方已离线，连接已结束。");
    if (raw.contains("ended", Qt::CaseInsensitive))
        return QStringLiteral("对方已结束会话。");
    return QStringLiteral("信令服务未能完成请求。请检查服务器地址及邀请码。");
}
} // namespace

void DesktopController::Dispatch::frame(const I420Frame& f) {
    std::lock_guard guard(lock);
    if (surface)
        surface->submitI420(f.width, f.height, f.y, f.strideY, f.u, f.strideU, f.v, f.strideV, f.rotation,
                            f.owner);
}
void DesktopController::Dispatch::decoded(const DecodedVideoFrame& frame, int rotation) {
    std::lock_guard guard(lock);
    if (!surface || !frame.submit(*surface, rotation))
        return;
    if (frame.isHardware())
        ++gpuDecoded;
    else
        ++cpuDecoded;
}

DesktopController::DesktopController(HomeWindow& home, HostAdapters adapters, QString deviceStorePath,
                                     const QUrl& initialAddress)
    : home_(home), hostAdapters_(std::move(adapters)) {
    connect(&home, &HomeWindow::joinRequested, this, &DesktopController::join);
    connect(&home, &HomeWindow::cancelRequested, this, &DesktopController::endSession);
    connect(&home, &HomeWindow::hostRequested, this, [this](CodeOptions options, bool highRefresh,
                                                            bool relayOnly, bool withAudio) {
        openSignaling(Role::Host, {}, options, highRefresh, relayOnly, withAudio);
    });
    connect(&home, &HomeWindow::hostApproved, this, &DesktopController::hostApproved);
    connect(&home, &HomeWindow::hostDenied, this, &DesktopController::hostDenied);
    connect(&home, &HomeWindow::hostEndRequested, this, &DesktopController::endSession);
    connect(&home, &HomeWindow::hostDesktopUnavailable, this, [this] {
        if (role_ == Role::Host)
            finish(QStringLiteral("Windows 交互桌面不可用，本机共享已停止。"), true, true, "display-unavailable");
    });
    connect(&home, &HomeWindow::sessionActivationRequested, this, [this] {
        if (window_) {
            window_->showNormal();
            window_->raise();
            window_->activateWindow();
        }
    });
    deadline_.setSingleShot(true);
    deadline_.setTimerType(Qt::PreciseTimer);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        if (role_ == Role::Host) {
            const bool invitation = phase_ == Phase::Inviting || phase_ == Phase::PendingApproval;
            finish(invitation ? QStringLiteral("邀请或授权请求已过期，请重新创建。")
                              : QStringLiteral("连接超时，本机共享已结束。"),
                   true);
            return;
        }
        if (phase_ == Phase::Waiting)
            finish(QStringLiteral("等待对方同意超时，请获取新的邀请码。"), true);
        else if (!rtc_)
            finish(QStringLiteral("无法连接到信令服务，请检查地址和网络。"), true);
        else if (!offered_)
            finish(QStringLiteral("等待对方的媒体协商信息超时。"), true);
        else if (!connected_)
            finish(QStringLiteral("未能建立媒体连接，请检查两端网络以及 STUN / TURN。"), true);
        else if (surface_ && !surface_->stats().presented)
            finish(QStringLiteral("连接已建立，但没有画面可供显示。"), true, true, "display-unavailable");
        else
            finish(QStringLiteral("系统声音未就绪，请对方检查共享音频设置。"), true, true,
                   "audio-unavailable");
    });
    sampler_.setInterval(500);
    connect(&sampler_, &QTimer::timeout, this, &DesktopController::updateStats);
    videoSettingsReply_.setSingleShot(true);
    connect(&videoSettingsReply_, &QTimer::timeout, this, [this] {
        window_->setVideoSettings(videoSettings_.maxBitrateMbps, videoSettings_.maxFps, true,
                                  QStringLiteral("对端未及时确认，当前显示上次确认的设置。"));
    });
    releaseRetry_.setInterval(500);
    connect(&releaseRetry_, &QTimer::timeout, this, &DesktopController::retryInputRelease);
    devices_ = std::make_unique<DeviceConnection>(*this, home, std::move(deviceStorePath), initialAddress);
}
DesktopController::~DesktopController() {
    endSession();
}
void DesktopController::join(const QString& invite) {
    if (invite.trimmed().startsWith(QStringLiteral("EVP1."), Qt::CaseInsensitive)) {
        home_.showFailure(QStringLiteral("这是配对码，请在「我的设备」中请求配对。"));
        return;
    }
    openSignaling(Role::Viewer, invite);
}
void DesktopController::openSignaling(Role role, const QString& invite, CodeOptions options,
                                      bool highRefresh, bool relayOnly, bool withAudio) {
    if (phase_ != Phase::Idle)
        return;
    if (devices_->busy()) {
        home_.showFailure(QStringLiteral("请先结束「我的设备」中的待处理请求。"));
        return;
    }
    const QUrl url = devices_->signalingUrl();
    const auto credential = role == Role::Host ? devices_->deploymentKey() : invite.trimmed();
    if (url.isEmpty() || (role == Role::Host && credential.isEmpty())) {
        home_.showSettingsPage();
        home_.setSettingsMessage(url.isEmpty() ? QStringLiteral("请先保存信令服务器地址。")
                                              : QStringLiteral("创建邀请码前，请检查部署密钥和本机存储状态。"));
        return;
    }
    if (credential.isEmpty()) {
        home_.showFailure(QStringLiteral("请输入对方发来的邀请码。"));
        return;
    }
    if (role == Role::Host) {
        if (!isCodeLifetime(options.minutes)) {
            home_.showFailure(QStringLiteral("请选择有效的邀请码期限。"));
            return;
        }
        if (options.autoApprove) {
            invitationSecret_ = deviceRandom();
            if (invitationSecret_.isEmpty()) {
                home_.showFailure(QStringLiteral("无法生成安全的邀请码，请重试。"));
                return;
            }
        }
        invitationOptions_ = options;
        highRefresh_ = highRefresh;
        relayOnly_ = relayOnly;
        withAudio_ = withAudio;
    }
    ++generation_;
    role_ = role;
    phase_ = Phase::Connecting;
    devices_->updateActivity();
    if (role == Role::Host) {
        if (!home_.setHostBusy(true)) {
            finish(QStringLiteral("无法监测 Windows 桌面状态。请返回已解锁的桌面后重试。"), true, false);
            return;
        }
    } else {
        home_.setBusy(true);
    }
    home_.setStatus(QStringLiteral("正在连接信令服务…"));
    socket_ = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
    socket_->setMaxAllowedIncomingMessageSize(65536);
    socket_->setMaxAllowedIncomingFrameSize(65536);
    connect(
        socket_, &QWebSocket::connected, this,
        [this, credential] {
            if (phase_ != Phase::Connecting)
                return;
            phase_ = Phase::Joining;
            QJsonObject request;
            if (role_ == Role::Host) {
                request = {{"type", "host"}, {"key", credential}};
                if (invitationOptions_.autoApprove || invitationOptions_.minutes != codeLifetimeMinutes.front())
                    request.insert("inviteTtlMinutes", invitationOptions_.minutes);
                if (invitationOptions_.autoApprove)
                    request.insert("inviteVerifier", invitationVerifier(invitationSecret_));
            } else {
                request = {{"type", "join"}, {"invite", credential.trimmed()}};
            }
            if (!sendSignal(request))
                finish(QStringLiteral("无法发送连接请求。"), true, false);
        },
        Qt::SingleShotConnection);
    connect(socket_, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
        const auto msg = QJsonDocument::fromJson(text.toUtf8());
        if (!msg.isObject()) {
            finish(QStringLiteral("收到无效的信令数据。"), true);
            return;
        }
        received(msg.object());
    });
    connect(socket_, &QWebSocket::disconnected, this,
            [this] { finish(QStringLiteral("信令连接已断开。"), true, false); });
    connect(socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        finish(QStringLiteral("信令连接失败，请检查网络、服务器地址和 TLS 证书。"), true, false);
    });
    deadline_.start(15000);
    socket_->open(url);
}
void DesktopController::invitationIssued(const QJsonObject& message) {
    const auto minutes = invitationOptions_.minutes;
    const bool automatic = invitationOptions_.autoApprove;
    const bool custom = automatic || minutes != codeLifetimeMinutes.front();
    if ((custom || message.contains("inviteTtlMinutes")) && message.value("inviteTtlMinutes") != minutes) {
        finish(QStringLiteral("信令服务未确认所选期限。请升级服务，或使用 5 分钟且需本机确认的邀请码。"), true);
        return;
    }
    QString code;
    if (automatic) {
        if (invitationSecret_.size() != 32 || message.value("inviteVerifier") != invitationVerifier(invitationSecret_) ||
            message.contains("invite")) {
            finish(QStringLiteral("信令服务未确认本次免确认邀请。请升级服务后重试。"), true);
            return;
        }
        code = encodeDeviceBytes(invitationSecret_);
    } else {
        code = message.value("invite").toString();
        if (code.isEmpty() || code.size() > 256 || message.contains("inviteVerifier")) {
            finish(QStringLiteral("信令服务未返回有效的邀请码。"), true);
            return;
        }
    }
    phase_ = Phase::Inviting;
    invitationAge_.start();
    deadline_.start(minutes * 60000);
    home_.setHostInvitation(code, minutes, automatic);
    home_.setStatus(automatic ? QStringLiteral("邀请码已就绪。首位持码者可在有效期内直接控制本机。")
                              : QStringLiteral("请将一次性邀请码发给对方，等待连接请求。"));
}

void DesktopController::invitationRedeemed(const QJsonObject& message) {
    if (!invitationAge_.isValid() || invitationAge_.elapsed() >= invitationOptions_.minutes * 60000) {
        finish(QStringLiteral("本次邀请码已过期，请重新创建。"), true);
        return;
    }
    invitationAge_.invalidate();
    phase_ = Phase::PendingApproval;
    if (!invitationOptions_.autoApprove) {
        home_.setHostPending(generation_);
        home_.setStatus(QStringLiteral("对方请求控制本机。请确认是否允许共享。"));
        deadline_.start(60000);
        return;
    }
    const auto proof = decodeDeviceBytes(message.value("inviteProof").toString(), 32);
    const bool verified = equalDeviceSecrets(proof, invitationSecret_);
    invitationSecret_.fill(0);
    invitationSecret_.clear();
    if (!verified) {
        finish(QStringLiteral("未能验证本次邀请码，请重新创建。"), true);
        return;
    }
    home_.setHostInvitation({}, invitationOptions_.minutes, true);
    hostApproved(generation_, highRefresh_, relayOnly_, withAudio_);
}

bool DesktopController::reserveDeviceSession(bool host, bool relayOnly, const DevicePreferences& preferences) {
    if (phase_ != Phase::Idle)
        return false;
    if (host) {
        retryInputRelease();
        if (releasingInput_)
            return false;
        auto input = makeHostInput();
        if (!input || !input->checkEnvironment().isEmpty() || !home_.setHostBusy(true, true))
            return false;
        localInput_ = std::move(input);
    }
    ++generation_;
    trustedSession_ = true;
    role_ = host ? Role::Host : Role::Viewer;
    phase_ = host ? Phase::Approved : Phase::Waiting;
    relayOnly_ = relayOnly;
    highRefresh_ = false;
    withAudio_ = preferences.audio;
    if (!host)
        home_.setBusy(true, true);
    return true;
}
bool DesktopController::sendSignal(QJsonObject message) {
    if (trustedSession_)
        return message.value("type") == "signal" && devices_->sendMedia(message.value("data").toObject());
    return socket_ && socket_->state() == QAbstractSocket::ConnectedState &&
           socket_->sendTextMessage(
               QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact))) >= 0;
}
void DesktopController::received(const QJsonObject& m) {
    const auto type = m.value("type").toString();
    if (type == "error" || type == "ended") {
        finish(peerMessage(m.value(type == "error" ? "message" : "reason").toString()), type == "error",
               false);
    } else if (type == "invitation" && role_ == Role::Host && phase_ == Phase::Joining) {
        invitationIssued(m);
    } else if (type == "request" && role_ == Role::Host && phase_ == Phase::Inviting) {
        invitationRedeemed(m);
    } else if (type == "waiting" && role_ == Role::Viewer && phase_ == Phase::Joining) {
        phase_ = Phase::Waiting;
        home_.setStatus(QStringLiteral("已发送请求，等待对方同意共享…"));
        deadline_.start(65000);
    } else if (type == "ready") {
        const bool authorized = role_ == Role::Host
                                    ? phase_ == Phase::Approved && m.value("role") == "host" &&
                                          m.value("relayOnly").isBool() &&
                                          m.value("relayOnly").toBool() == relayOnly_
                                    : phase_ == Phase::Waiting && m.value("role") == "viewer";
        if (!authorized) {
            finish(QStringLiteral("授权状态不一致，请重新邀请。"), true);
            return;
        }
        ready(m);
    } else if (type == "signal" && (rtc_ || host_)) {
        const auto data = m.value("data").toObject();
        if (data.contains("description")) {
            const auto d = data.value("description").toObject();
            const auto sdp = d.value("sdp").toString();
            if (offered_ || d.value("type") != (role_ == Role::Host ? "answer" : "offer") || sdp.isEmpty()) {
                finish(QStringLiteral("对方的媒体协商信息无效。"), true);
                return;
            }
            offered_ = true;
            if (host_)
                host_->acceptAnswer(sdp.toStdString());
            else {
                withAudio_ = sdp.contains(QStringLiteral("\nm=audio "));
                rtc_->acceptOffer(sdp.toStdString());
            }
        } else if (data.contains("candidate")) {
            const auto c = data.value("candidate").toObject();
            const auto line = c.value("candidate").toString();
            if (!line.isEmpty()) {
                IceCandidate candidate{line.toStdString(), c.value("sdpMid").toString().toStdString(),
                                       c.value("sdpMLineIndex").toInt()};
                if (host_)
                    host_->addRemoteCandidate(std::move(candidate));
                else
                    rtc_->addRemoteCandidate(std::move(candidate));
            }
        }
    }
}
void DesktopController::ready(const QJsonObject& m) {
    phase_ = Phase::Negotiating;
    relayOnly_ = m.value("relayOnly").toBool();
    std::vector<IceServer> servers;
    for (const auto& entry : m.value("iceServers").toArray()) {
        const auto obj = entry.toObject();
        IceServer server;
        for (const auto& u : obj.value("urls").toArray())
            server.urls.push_back(u.toString().toStdString());
        server.username = obj.value("username").toString().toStdString();
        server.credential = obj.value("credential").toString().toStdString();
        if (!server.urls.empty())
            servers.push_back(std::move(server));
    }
    if (role_ == Role::Host) {
        hostReady(servers);
        return;
    }
    surface_ = new VideoSurface;
    surface_->setObjectName("remoteSurface");
    window_ = new SessionWindow(surface_);
    input_ = new ViewerInput(surface_, [this](QJsonObject e) { return sendInput(std::move(e)); }, this);
    connect(window_, &SessionWindow::endRequested, this, &DesktopController::endSession);
    connect(window_, &SessionWindow::videoSettingsRequested, this, &DesktopController::requestVideoSettings);
    window_->setVideoSettings(videoSettings_.maxBitrateMbps, videoSettings_.maxFps, false,
                              QStringLiteral("连接后可查看对端支持的画面设置。"));
    window_->setStatus(QStringLiteral("正在协商音视频连接…"));
    window_->show();
    home_.setSessionActive(true);
    home_.setStatus(trustedSession_ ? QStringLiteral("设备身份已验证，正在建立远程会话…")
                                    : QStringLiteral("对方已同意，正在建立远程会话…"));
    dispatch_ = std::make_shared<Dispatch>(this, surface_, generation_);
    const auto gate = dispatch_;
    RtcViewer::Callbacks cb;
    cb.post = [gate](std::function<void()> f) { gate->post([f = std::move(f)](auto&) { f(); }); };
    cb.localAnswer = [gate](std::string sdp) {
        gate->post([sdp = QString::fromStdString(sdp)](DesktopController& c) {
            const bool sent = c.sendSignal(
                {{"type", "signal"},
                 {"data", QJsonObject{{"description", QJsonObject{{"type", "answer"}, {"sdp", sdp}}}}}});
            if (!sent && c.trustedSession_)
                c.finish(QStringLiteral("无法发送设备会话协商信息。"), true);
        });
    };
    cb.localCandidate = [gate](IceCandidate i) {
        gate->post([i = std::move(i)](DesktopController& c) mutable { c.forwardCandidate(std::move(i)); });
    };
    cb.connectionState = [gate](std::string s) {
        gate->post([s = std::move(s)](DesktopController& c) { c.stateChanged(s); });
    };
    cb.inputOpen = [gate](bool open) {
        gate->post([open](DesktopController& c) {
            c.channel_ = open;
            if (!open && c.everOpened_) {
                c.finish(QStringLiteral("键鼠通道已关闭，会话已结束。"), true);
                return;
            }
            c.everOpened_ |= open;
            c.updatePermit();
        });
    };
    cb.inputMessage = [gate](std::string raw) {
        gate->post([raw = std::move(raw)](DesktopController& c) {
            static const QRegularExpression pattern(QStringLiteral("^input-ready:([1-9][0-9]{0,9})$"));
            const auto match = pattern.match(QString::fromStdString(raw));
            if (match.hasMatch()) {
                const auto epoch = match.captured(1).toULongLong();
                c.input_->release();
                if (c.epoch_ != epoch) {
                    c.epoch_ = epoch;
                    c.remoteVideoSettings_ = false;
                    c.videoSettingsReply_.stop();
                    c.window_->setVideoSettings(c.videoSettings_.maxBitrateMbps, c.videoSettings_.maxFps, false,
                                                QStringLiteral("需要对端支持会话内画面设置。"));
                }
                c.updatePermit();
            } else
                c.receiveVideoSettings(QJsonDocument::fromJson(QByteArray::fromStdString(raw)).object());
        });
    };
    cb.videoFrame = [gate](const I420Frame& frame) { gate->frame(frame); };
    cb.decodedFrame = [gate](const DecodedVideoFrame& frame, int rotation) {
        gate->decoded(frame, rotation);
    };
    cb.routeStats = [gate](RouteStats r) {
        gate->post([r = std::move(r)](DesktopController& c) {
            if (c.relayOnly_ && r.kind == "direct") {
                c.finish(QStringLiteral("当前连接与要求的中继策略不符。"), true);
                return;
            }
            c.route_ = QString::fromStdString(r.kind);
            c.window_->setRoute(r.kind == "direct"  ? QStringLiteral("直连")
                                : r.kind == "relay" ? QStringLiteral("中继")
                                                    : QStringLiteral("检测中"));
        });
    };
    cb.error = [gate](std::string) {
        gate->post([](DesktopController& c) {
            c.finish(QStringLiteral("媒体协商失败，请检查网络、编解码支持和中继配置。"), true);
        });
    };
    cb.audioTrack = [gate] { gate->post([](DesktopController& c) { c.audio_ = true; }); };
    cb.audioPlayout = [gate] { gate->post([](DesktopController& c) { c.playout_ = true; }); };
    rtc_ = std::make_unique<RtcViewer>(std::move(cb));
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    device.Attach(surface_->acquireD3D11Device());
    if (!rtc_->start(servers, relayOnly_, device.Get())) {
        finish(QStringLiteral("无法初始化原生音视频引擎。"), true);
        return;
    }
    interval_.start();
    sampler_.start();
    deadline_.start(45000);
}
void DesktopController::stateChanged(const std::string& state) {
    if (state == "failed" || state == "closed") {
        finish(QStringLiteral("媒体连接已中断。"), true);
        return;
    }
    if (state == "disconnected") {
        input_->setAvailable(false);
        connected_ = false;
        videoSettingsReply_.stop();
        window_->setVideoSettings(videoSettings_.maxBitrateMbps, videoSettings_.maxFps, false,
                                  QStringLiteral("网络中断，等待重新连接。"));
        route_.clear();
        window_->setConnected(false);
        window_->setInputAvailable(false);
        window_->setRoute(QStringLiteral("检测中"));
        window_->setStatus(QStringLiteral("网络已中断，正在恢复连接…"));
        deadline_.start(8000);
    } else if (state == "connected") {
        connected_ = true;
        phase_ = Phase::Streaming;
        window_->setConnected(true);
        window_->setVideoSettings(videoSettings_.maxBitrateMbps, videoSettings_.maxFps, remoteVideoSettings_,
                                  remoteVideoSettings_ ? QString{} : QStringLiteral("需要对端支持会话内画面设置。"));
        window_->setStatus(QStringLiteral("已连接，等待远程画面…"));
        home_.setStatus(trustedSession_ ? QStringLiteral("可信设备会话进行中")
                                        : QStringLiteral("远程协助会话进行中"));
        deadline_.start(45000);
        updatePermit();
        rtc_->pollStats();
    }
}
void DesktopController::updateStats() {
    if (role_ == Role::Host) {
        tickHost();
        return;
    }
    if (!rtc_ || !surface_)
        return;
    const auto stats = surface_->stats();
    const auto ms = interval_.restart();
    qCDebug(videoTiming) << "viewer" << "decoded-gpu" << dispatch_->gpuDecoded.load()
                         << "decoded-cpu" << dispatch_->cpuDecoded.load() << "submitted" << stats.submitted
                         << "replaced" << stats.replaced << "presented" << stats.presented
                         << "present-busy" << stats.presentBusy << "last-delivery-to-present-us" << stats.lastPresentDelayUs;
    const auto fps = ms > 0 ? (stats.presented - presented_) * 1000.0 / ms : 0.0;
    presented_ = stats.presented;
    if (presented_) {
        window_->setVideoFormat(surface_->sourceSize(), fps);
        if (connected_) {
            window_->setStatus(!withAudio_ ? QStringLiteral("远程会话进行中 · 对方未共享声音")
                               : playout_  ? QStringLiteral("远程会话进行中")
                               : audio_    ? QStringLiteral("已收到音频，等待本机播放")
                                           : QStringLiteral("已收到画面，等待系统声音…"));
            if (!withAudio_ || audio_)
                deadline_.stop();
        }
    }
    updatePermit();
    if (connected_)
        rtc_->pollStats();
    emit sample(presented_, surface_->sourceSize(), route_, audio_,
                connected_ && channel_ && epoch_ && presented_, dispatch_->gpuDecoded.load(),
                dispatch_->cpuDecoded.load());
}
void DesktopController::updatePermit() {
    const bool allowed =
        !inputFault_ && connected_ && channel_ && epoch_ && surface_ && surface_->stats().presented;
    input_->setAvailable(allowed);
    window_->setInputAvailable(allowed);
}
void DesktopController::requestVideoSettings(int maxBitrateMbps, int maxFps) {
    const VideoSettings choice{maxBitrateMbps, maxFps};
    if (!connected_ || !channel_ || !epoch_ || !remoteVideoSettings_ || !choice.valid() ||
        videoSettingsReply_.isActive() || choice == videoSettings_)
        return;
    const auto request = QJsonDocument(QJsonObject{{"type", "video-settings"},
        {"epoch", static_cast<qint64>(epoch_)}, {"serial", ++videoSettingsSerial_},
        {"mbps", choice.maxBitrateMbps}, {"fps", choice.maxFps}}).toJson(QJsonDocument::Compact);
    const bool sent = rtc_->sendInput(request.toStdString());
    if (sent)
        videoSettingsReply_.start(3000);
    window_->setVideoSettings(videoSettings_.maxBitrateMbps, videoSettings_.maxFps, !sent,
                              sent ? QStringLiteral("正在应用…") : QStringLiteral("发送失败，请重试。"));
}
void DesktopController::receiveVideoSettings(const QJsonObject& message) {
    if (message.value("type") != "video-settings" || !epoch_ ||
        message.value("epoch").toInteger() != static_cast<qint64>(epoch_))
        return;
    const auto serial = message.value("serial").toInt(-1);
    const auto applied = message.value("applied");
    const VideoSettings confirmed{message.value("mbps").toInt(-1), message.value("fps").toInt(-1)};
    if (!confirmed.valid() || !applied.isBool() ||
        (serial == 0 ? (remoteVideoSettings_ || !applied.toBool())
                     : (!remoteVideoSettings_ || serial != videoSettingsSerial_)))
        return;
    remoteVideoSettings_ = true;
    videoSettings_ = confirmed;
    videoSettingsReply_.stop();
    window_->setVideoSettings(confirmed.maxBitrateMbps, confirmed.maxFps, connected_,
                              applied.toBool() ? QString{} : QStringLiteral("对端未能应用，已保留原设置。"));
}
bool DesktopController::sendInput(QJsonObject e) {
    if (inputFault_ || !rtc_ || !channel_ || !epoch_ || !connected_)
        return false;
    e.insert("epoch", static_cast<qint64>(epoch_));
    const auto bytes = QJsonDocument(e).toJson(QJsonDocument::Compact);
    const bool sent = rtc_->sendInput(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));
    if (!sent && e.value("type") != "move" && !inputFault_ && phase_ != Phase::Idle) {
        // A lost key/button transition invalidates the pressed state. Close after this
        // input event returns so the host releases its adapter and any held keys.
        inputFault_ = true;
        const auto current = generation_;
        QTimer::singleShot(0, this, [this, current] {
            if (generation_ == current)
                finish(QStringLiteral("键鼠通道发送失败，已结束控制并释放按键。"), true);
        });
    }
    return sent;
}
void DesktopController::endSession() {
    finish(QStringLiteral("会话已结束。"), false);
}
void DesktopController::forwardCandidate(IceCandidate i) {
    const QJsonObject candidate{{"candidate", QString::fromStdString(i.candidate)},
                                {"sdpMid", QString::fromStdString(i.sdpMid)},
                                {"sdpMLineIndex", i.sdpMLineIndex}};
    if (!sendSignal({{"type", "signal"}, {"data", QJsonObject{{"candidate", candidate}}}}) && trustedSession_)
        finish(QStringLiteral("无法发送设备会话网络信息。"), true);
}
void DesktopController::finish(const QString& message, bool failure, bool notify, QString reason) {
    if (phase_ == Phase::Idle)
        return;
    phase_ = Phase::Idle;
    invitationAge_.invalidate();
    invitationOptions_ = {};
    invitationSecret_.fill(0);
    invitationSecret_.clear();
    ++generation_;
    deadline_.stop();
    sampler_.stop();
    videoSettingsReply_.stop();
    if (input_)
        input_->setAvailable(false);
    if (dispatch_)
        dispatch_->detach();
    if (localInput_ && !revokeHostInput()) {
        // A lock screen can temporarily reject key-up. This sink is permanently disarmed
        // and can only retry the transitions that it previously sent successfully.
        releasingInput_ = std::move(localInput_);
        releaseRetry_.start();
    }
    const bool trusted = std::exchange(trustedSession_, false);
    if (notify && !trusted) {
        QJsonObject end{{"type", "end"}};
        if (failure)
            end.insert("reason", reason.isEmpty() ? QStringLiteral("transport-failed") : reason);
        sendSignal(end);
    }
    if (socket_) {
        socket_->disconnect(this);
        socket_->close();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (rtc_) {
        rtc_->close();
        rtc_.reset();
    }
    if (host_) {
        host_->close();
        host_.reset();
    }
    localInput_.reset();
    dispatch_.reset();
    delete input_;
    input_ = nullptr;
    if (window_) {
        window_->finish();
        window_->deleteLater();
        window_ = nullptr;
    }
    surface_ = nullptr;
    route_.clear();
    presented_ = epoch_ = 0;
    connected_ = channel_ = audio_ = offered_ = relayOnly_ = everOpened_ = playout_ = inputFault_ = false;
    if (role_ == Role::Host)
        home_.setHostBusy(false);
    else
        home_.setBusy(false);
    hostArmed_ = highRefresh_ = false;
    hostAudioUnavailable_ = false;
    withAudio_ = true;
    encoder_.clear();
    captureSize_ = {};
    videoSettings_ = {};
    remoteVideoSettings_ = false;
    videoSettingsSerial_ = 0;
    if (failure)
        home_.showFailure(message);
    else
        home_.setStatus(message);
    if (trusted)
        devices_->sessionEnded(message, notify, reason == "declined");
    else
        devices_->updateActivity();
    emit sessionFinished(message);
}
} // namespace evp
