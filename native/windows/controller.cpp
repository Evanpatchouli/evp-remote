#include "controller.h"
#include "media/video_surface.h"
#include "media/ffmpeg_decoder.h"
#include "ui/home_window.h"
#include "ui/session_window.h"
#include "viewer_input.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QWebSocket>
#include <mutex>
#include <atomic>
#include <utility>
#include <d3d11.h>
#include <wrl/client.h>

namespace evp {
namespace {
QString peerMessage(const QString& raw) {
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
bool validUrl(const QUrl& u) {
    const bool local = u.host() == "localhost" || u.host() == "127.0.0.1" || u.host() == "::1";
    return u.isValid() && !u.host().isEmpty() && (u.scheme() == "wss" || (u.scheme() == "ws" && local)) &&
           u.userInfo().isEmpty() && !u.hasQuery() && !u.hasFragment() &&
           (u.path().isEmpty() || u.path() == "/");
}
} // namespace

// SDK callbacks can outlive a session. Detach the target before closing the peer or either window.
struct DesktopController::Dispatch {
    std::mutex lock;
    DesktopController* target;
    VideoSurface* surface;
    quint64 generation;
    std::atomic<quint64> gpuDecoded{0}, cpuDecoded{0};
    Dispatch(DesktopController* c, VideoSurface* v, quint64 g) : target(c), surface(v), generation(g) {}
    template <class F> void post(F work) {
        std::lock_guard guard(lock);
        if (!target)
            return;
        QMetaObject::invokeMethod(
            target,
            [c = target, g = generation, work = std::move(work)]() mutable {
                if (c->generation_ == g)
                    work(*c);
            },
            Qt::QueuedConnection);
    }
    void frame(const I420Frame& f) {
        std::lock_guard guard(lock);
        if (surface)
            surface->submitI420(f.width, f.height, f.y, f.strideY, f.u, f.strideU, f.v, f.strideV, f.rotation,
                                f.owner);
    }
    void decoded(const DecodedVideoFrame& frame, int rotation) {
        std::lock_guard guard(lock);
        if (!surface || !frame.submit(*surface, rotation))
            return;
        if (frame.isHardware())
            ++gpuDecoded;
        else
            ++cpuDecoded;
    }
    void detach() {
        std::lock_guard guard(lock);
        target = nullptr;
        surface = nullptr;
    }
};

DesktopController::DesktopController(HomeWindow& home) : home_(home) {
    connect(&home, &HomeWindow::joinRequested, this, &DesktopController::join);
    connect(&home, &HomeWindow::cancelRequested, this, &DesktopController::endSession);
    connect(&home, &HomeWindow::sessionActivationRequested, this, [this] {
        if (window_) {
            window_->showNormal();
            window_->raise();
            window_->activateWindow();
        }
    });
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
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
}
DesktopController::~DesktopController() {
    endSession();
}
void DesktopController::join(const QUrl& url, const QString& invite) {
    if (phase_ != Phase::Idle)
        return;
    if (!validUrl(url) || invite.trimmed().isEmpty()) {
        home_.showFailure(QStringLiteral("请填写有效的信令地址和邀请码。"));
        return;
    }
    ++generation_;
    phase_ = Phase::Connecting;
    home_.setBusy(true);
    home_.setStatus(QStringLiteral("正在连接信令服务…"));
    QSettings().setValue("signaling/url", url.toString());
    socket_ = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
    socket_->setMaxAllowedIncomingMessageSize(65536);
    socket_->setMaxAllowedIncomingFrameSize(65536);
    connect(socket_, &QWebSocket::connected, this, [this, invite] {
        if (phase_ != Phase::Connecting)
            return;
        phase_ = Phase::Joining;
        if (!sendSignal({{"type", "join"}, {"invite", invite.trimmed()}}))
            finish(QStringLiteral("无法发送连接请求。"), true, false);
    });
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
bool DesktopController::sendSignal(QJsonObject message) {
    return socket_ && socket_->state() == QAbstractSocket::ConnectedState &&
           socket_->sendTextMessage(
               QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact))) >= 0;
}
void DesktopController::received(const QJsonObject& m) {
    const auto type = m.value("type").toString();
    if (type == "error" || type == "ended") {
        finish(peerMessage(m.value(type == "error" ? "message" : "reason").toString()), type == "error",
               false);
    } else if (type == "waiting" && phase_ == Phase::Joining) {
        phase_ = Phase::Waiting;
        home_.setStatus(QStringLiteral("已发送请求，等待对方同意共享…"));
        deadline_.start(65000);
    } else if (type == "ready") {
        if (phase_ != Phase::Waiting || m.value("role") != "viewer") {
            finish(QStringLiteral("授权状态不一致，请重新邀请。"), true);
            return;
        }
        ready(m);
    } else if (type == "signal" && rtc_) {
        const auto data = m.value("data").toObject();
        if (data.contains("description")) {
            const auto d = data.value("description").toObject();
            const auto sdp = d.value("sdp").toString();
            if (offered_ || d.value("type") != "offer" || sdp.isEmpty()) {
                finish(QStringLiteral("对方的媒体协商信息无效。"), true);
                return;
            }
            offered_ = true;
            rtc_->acceptOffer(sdp.toStdString());
        } else if (data.contains("candidate")) {
            const auto c = data.value("candidate").toObject();
            const auto line = c.value("candidate").toString();
            if (!line.isEmpty())
                rtc_->addRemoteCandidate({line.toStdString(), c.value("sdpMid").toString().toStdString(),
                                          c.value("sdpMLineIndex").toInt()});
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
    surface_ = new VideoSurface;
    surface_->setObjectName("remoteSurface");
    window_ = new SessionWindow(surface_);
    input_ = new ViewerInput(surface_, [this](QJsonObject e) { return sendInput(std::move(e)); }, this);
    connect(window_, &SessionWindow::endRequested, this, &DesktopController::endSession);
    window_->setStatus(QStringLiteral("正在协商音视频连接…"));
    window_->show();
    home_.setSessionActive(true);
    home_.setStatus(QStringLiteral("对方已同意，正在建立远程会话…"));
    dispatch_ = std::make_shared<Dispatch>(this, surface_, generation_);
    const auto gate = dispatch_;
    RtcViewer::Callbacks cb;
    cb.post = [gate](std::function<void()> f) { gate->post([f = std::move(f)](auto&) { f(); }); };
    cb.localAnswer = [gate](std::string sdp) {
        gate->post([sdp = QString::fromStdString(sdp)](DesktopController& c) {
            c.sendSignal(
                {{"type", "signal"},
                 {"data", QJsonObject{{"description", QJsonObject{{"type", "answer"}, {"sdp", sdp}}}}}});
        });
    };
    cb.localCandidate = [gate](IceCandidate i) {
        gate->post([i = std::move(i)](DesktopController& c) {
            c.sendSignal(
                {{"type", "signal"},
                 {"data",
                  QJsonObject{{"candidate", QJsonObject{{"candidate", QString::fromStdString(i.candidate)},
                                                        {"sdpMid", QString::fromStdString(i.sdpMid)},
                                                        {"sdpMLineIndex", i.sdpMLineIndex}}}}}});
        });
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
                c.input_->release();
                c.epoch_ = match.captured(1).toULongLong();
                c.updatePermit();
            }
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
        window_->setStatus(QStringLiteral("已连接，等待远程画面…"));
        home_.setStatus(QStringLiteral("远程协助会话进行中"));
        deadline_.start(45000);
        updatePermit();
        rtc_->pollStats();
    }
}
void DesktopController::updateStats() {
    if (!rtc_ || !surface_)
        return;
    const auto stats = surface_->stats();
    const auto ms = interval_.restart();
    const auto fps = ms > 0 ? (stats.presented - presented_) * 1000.0 / ms : 0.0;
    presented_ = stats.presented;
    if (presented_) {
        window_->setVideoFormat(surface_->sourceSize(), fps);
        if (connected_) {
            window_->setStatus(playout_ ? QStringLiteral("远程会话进行中")
                               : audio_ ? QStringLiteral("已收到音频，等待本机播放")
                                        : QStringLiteral("已收到画面，等待系统声音…"));
            if (audio_)
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
void DesktopController::finish(const QString& message, bool failure, bool notify, QString reason) {
    if (phase_ == Phase::Idle)
        return;
    phase_ = Phase::Idle;
    ++generation_;
    deadline_.stop();
    sampler_.stop();
    if (input_)
        input_->setAvailable(false);
    if (dispatch_)
        dispatch_->detach();
    if (notify) {
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
    home_.setBusy(false);
    if (failure)
        home_.showFailure(message);
    else
        home_.setStatus(message);
    emit sessionFinished(message);
}
} // namespace evp
