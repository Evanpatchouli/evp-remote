#include "controller.h"
#include "controller_dispatch.h"
#include "host_input.h"
#include "rtc/rtc_host.h"
#include "ui/home_window.h"
#include "video_settings_wire.h"
#include <QJsonDocument>
#include <QStringList>
#include <utility>

namespace evp {
void DesktopController::hostApproved(quint64 requestId, bool highRefresh, bool relayOnly, bool withAudio) {
    if (role_ != Role::Host || phase_ != Phase::PendingApproval || requestId != generation_)
        return;
    phase_ = Phase::Approved;
    highRefresh_ = highRefresh;
    relayOnly_ = relayOnly;
    withAudio_ = withAudio;
    home_.setStatus(QStringLiteral("已同意本次请求，等待建立共享连接…"));
    deadline_.start(15000);
    if (!sendSignal({{"type", "accept"}, {"relayOnly", relayOnly}}))
        finish(QStringLiteral("无法发送授权，本机未开始共享。"), true, false);
}

void DesktopController::hostDenied(quint64 requestId) {
    if (role_ != Role::Host || phase_ != Phase::PendingApproval || requestId != generation_)
        return;
    sendSignal({{"type", "reject"}});
    finish(QStringLiteral("已拒绝本次请求。"), false, false);
}

std::unique_ptr<HostInput> DesktopController::makeHostInput() const {
    return hostAdapters_.input ? hostAdapters_.input() : std::make_unique<HostInput>();
}

void DesktopController::hostReady(const std::vector<IceServer>& servers) {
    retryInputRelease();
    if (releasingInput_) {
        finish(QStringLiteral("上次共享仍有按键等待释放。请解锁并返回普通桌面后重试。"), true, true,
               "input-unavailable");
        return;
    }
    dispatch_ = std::make_shared<Dispatch>(this, nullptr, generation_);
    const auto gate = dispatch_;
    if (!localInput_)
        localInput_ = makeHostInput();
    if (!localInput_ || !localInput_->checkEnvironment().isEmpty()) {
        finish(QStringLiteral("请在单个活动显示器的交互桌面上共享本机。"), true, true, "display-unavailable");
        return;
    }
    RtcHost::Callbacks cb;
    cb.post = [gate](std::function<void()> work) { gate->post([work = std::move(work)](auto&) { work(); }); };
    cb.localOffer = [gate](std::string offer) {
        gate->post([offer = QString::fromStdString(offer)](DesktopController& c) {
            const QJsonObject description{{"type", "offer"}, {"sdp", offer}};
            if (!c.sendSignal({{"type", "signal"}, {"data", QJsonObject{{"description", description}}}}))
                c.finish(QStringLiteral("无法发送共享连接信息。"), true);
        });
    };
    cb.localCandidate = [gate](IceCandidate i) {
        gate->post([i = std::move(i)](DesktopController& c) mutable { c.forwardCandidate(std::move(i)); });
    };
    cb.connectionState = [this](std::string state) { hostStateChanged(state); };
    cb.inputOpen = [this](bool open) { hostChannelChanged(open); };
    // RtcHost bounds the queue and checks transport generation at owner-thread delivery.
    // Apply the command in this delivery, so a stale event cannot survive a reconnect.
    cb.inputMessage = [this, gate](std::string payload) {
        const auto grant = gate->inputGrant.load();
        if (grant && !payload.empty() && payload.size() <= 256)
            applyHostInput(grant, QByteArray::fromStdString(payload));
    };
    cb.routeStats = [gate](RouteStats stats) {
        gate->post([stats = std::move(stats)](DesktopController& c) {
            if (!c.connected_)
                return;
            if (c.relayOnly_ && stats.kind == "direct") {
                c.finish(QStringLiteral("连接与要求的中继策略不符，共享已停止。"), true);
                return;
            }
            c.route_ = QString::fromStdString(stats.kind);
            c.updateHostStatus();
        });
    };
    cb.videoSize = [gate](int w, int h) {
        gate->post([w, h](DesktopController& c) {
            c.captureSize_ = QSize(w, h);
            c.updateHostStatus();
        });
    };
    cb.encoder = [gate](std::string name) {
        gate->post([name = QString::fromStdString(name)](DesktopController& c) {
            c.encoder_ = name;
            c.updateHostStatus();
        });
    };
    cb.error = [gate](std::string detail) {
        gate->inputGrant = 0;
        gate->post([detail = std::move(detail)](DesktopController& c) {
            if (detail.find("audio") != std::string::npos || detail.find("WASAPI") != std::string::npos)
                c.finish(QStringLiteral("无法采集系统声音，请检查默认扬声器后重试。"), true, true,
                         "audio-unavailable");
            else
                c.finish(QStringLiteral("共享引擎无法继续运行，请检查显示器、编码设备和网络。"), true);
        });
    };
    cb.audioUnavailable = [this] {
        hostAudioUnavailable_ = true;
        if (connected_)
            updateHostStatus();
        else
            home_.setStatus(QStringLiteral("系统声音不可用，正在建立屏幕共享…"));
    };
    cb.audioError = [gate](std::string) {
        gate->inputGrant = 0;
        gate->post([](DesktopController& c) {
            c.finish(QStringLiteral("系统声音设备不可用，共享已停止。"), true, true, "audio-unavailable");
        });
    };
    host_ = std::make_unique<RtcHost>(std::move(cb), hostAdapters_.media ? hostAdapters_.media() : nullptr);
    videoSettings_ = {.maxFps = highRefresh_ ? 144 : 60};
    home_.setHostActive(true);
    home_.setStatus(withAudio_ ? QStringLiteral("正在准备共享本机屏幕和系统声音…")
                               : QStringLiteral("正在共享本机屏幕，等待对方连接…"));
    if (!host_->start(servers, relayOnly_, withAudio_, videoSettings_)) {
        finish(QStringLiteral("无法启动共享。请检查主显示器和编码设备后重试。"), true);
        return;
    }
    sampler_.start();
    deadline_.start(45000);
}

void DesktopController::hostStateChanged(const std::string& state) {
    if (state == "failed" || state == "closed") {
        finish(QStringLiteral("对端媒体连接已断开，共享已结束。"), true);
    } else if (state == "disconnected") {
        connected_ = false;
        revokeHostInput();
        route_.clear();
        home_.setStatus(QStringLiteral("网络中断，已释放键鼠，等待重连…"));
        deadline_.start(8000);
    } else if (state == "connected") {
        connected_ = true;
        phase_ = Phase::Streaming;
        // Require the input channel to open as well before declaring setup complete.
        if (!hostArmed_)
            deadline_.start(45000);
        grantHostInput();
        updateHostStatus();
    }
}
void DesktopController::hostChannelChanged(bool open) {
    channel_ = open;
    if (!open && everOpened_) {
        finish(QStringLiteral("键鼠通道已关闭，本机共享已结束。"), true, true, "input-unavailable");
        return;
    }
    everOpened_ |= open;
    grantHostInput();
}
void DesktopController::grantHostInput() {
    if (!connected_ || !channel_ || hostArmed_ || !localInput_ || !host_ || phase_ != Phase::Streaming)
        return;
    if (const auto error = localInput_->arm(); !error.isEmpty()) {
        finish(error, true, true, "input-unavailable");
        return;
    }
    hostArmed_ = true;
    dispatch_->inputGrant = ++epoch_;
    if (!host_->sendInput("input-ready:" + std::to_string(epoch_))) {
        finish(QStringLiteral("无法授予本次键鼠权限。"), true, true, "input-unavailable");
        return;
    }
    sendVideoSettings(0, true);
    deadline_.stop();
    updateHostStatus();
}
bool DesktopController::revokeHostInput() {
    if (dispatch_)
        dispatch_->inputGrant = 0;
    hostArmed_ = false;
    return !localInput_ || localInput_->disarm();
}
void DesktopController::retryInputRelease() {
    if (releasingInput_ && releasingInput_->disarm())
        releasingInput_.reset();
    if (!releasingInput_)
        releaseRetry_.stop();
}
void DesktopController::applyHostInput(quint64 grant, const QByteArray& json) {
    if (!hostArmed_ || !channel_ || !connected_ || !localInput_ || !dispatch_ || phase_ != Phase::Streaming ||
        grant != epoch_ || grant != dispatch_->inputGrant)
        return;
    const auto obj = QJsonDocument::fromJson(json).object();
    if (!obj.value("epoch").isDouble() || obj.value("epoch").toDouble() != static_cast<double>(grant))
        return;
    if (obj.value("type") == "video-settings") {
        const auto serial = obj.value("serial").toInt();
        if (serial > 0) {
            const auto requested = obj.value("adaptation");
            const auto adaptation = parseVideoAdaptation(requested);
            // Legacy viewers can change the ceilings without resetting the current policy.
            const VideoSettings choice{obj.value("mbps").toInt(-1), obj.value("fps").toInt(-1),
                                       adaptation.value_or(videoSettings_.adaptation)};
            const bool applied = (requested.isUndefined() || adaptation) && choice.valid() &&
                                 (choice == videoSettings_ || host_->setVideoSettings(choice));
            if (applied)
                videoSettings_ = choice;
            sendVideoSettings(serial, applied);
        }
        return;
    }
    if (localInput_->apply(obj) == HostInput::Result::Failed)
        finish(QStringLiteral("本机无法执行键鼠输入，已停止共享并释放按键。"), true, true,
               "input-unavailable");
}
void DesktopController::sendVideoSettings(int serial, bool applied) {
    const auto state = QJsonDocument(QJsonObject{{"type", "video-settings"},
        {"epoch", static_cast<qint64>(epoch_)}, {"serial", serial}, {"applied", applied},
        {"mbps", videoSettings_.maxBitrateMbps}, {"fps", videoSettings_.maxFps},
        {"adaptation", videoAdaptationName(videoSettings_.adaptation)}});
    host_->sendInput(state.toJson(QJsonDocument::Compact).toStdString());
}
void DesktopController::tickHost() {
    if (!host_)
        return;
    if (localInput_ && !localInput_->checkEnvironment().isEmpty()) {
        finish(QStringLiteral("桌面或显示设备已切换，本机共享已停止。"), true, true, "display-unavailable");
        return;
    }
    if (connected_)
        host_->pollStats();
}
void DesktopController::updateHostStatus() {
    if (!host_ || !connected_)
        return;
    QStringList details{hostArmed_ ? QStringLiteral("对方正在控制本机") : QStringLiteral("本机屏幕共享中")};
    if (route_ == "direct")
        details << QStringLiteral("直连");
    else if (route_ == "relay")
        details << QStringLiteral("中继");
    if (captureSize_.isValid())
        details << QStringLiteral("%1 × %2").arg(captureSize_.width()).arg(captureSize_.height());
    if (!encoder_.isEmpty())
        details << encoder_;
    if (hostAudioUnavailable_)
        details << QStringLiteral("系统声音不可用");
    home_.setStatus(details.join(QStringLiteral(" · ")));
}
} // namespace evp
