#include "controller.h"
#include "device_probe.h"
#include "host_input.h"
#include "rtc/rtc_host.h"
#include "ui/home_window.h"
#include "ui/system_tray.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTemporaryDir>
#include <windows.h>
#include <webrtc_c/api/video/adapted_video_track_source.h>
#include <webrtc_c/api/video/i420_buffer.h>
#include <webrtc_c/rtc_base/time_utils.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
using namespace std::chrono_literals;
struct Observations {
    int starts = 0, audioStarts = 0, stops = 0, armingChecks = 0;
    int down = 0, up = 0, move = 0, wheel = 0, leftDown = 0, leftUp = 0, heldDown = 0, heldUp = 0;
    int startingFps = 0;
    std::vector<int> liveFrameRates;
    bool receivedInput() const {
        return down && up && move && wheel && leftDown && leftUp;
    }
};
void publish(webrtc_AdaptedVideoTrackSource* source, unsigned counter) {
    constexpr int w = 640, h = 360;
    auto* ref = webrtc_I420Buffer_CreateWithStrides(w, h, w, w / 2, w / 2);
    auto* planes = webrtc_I420Buffer_refcounted_get(ref);
    std::memset(webrtc_I420Buffer_MutableDataY(planes), 32 + (counter % 160), w * h);
    std::memset(webrtc_I420Buffer_MutableDataU(planes), 110, w * h / 4);
    std::memset(webrtc_I420Buffer_MutableDataV(planes), 145, w * h / 4);
    auto* video = webrtc_I420Buffer_refcounted_cast_to_webrtc_VideoFrameBuffer(ref);
    auto* builder = webrtc_VideoFrameBuilder_new(video);
    auto* spec = webrtc_VideoFrameBuilder_unique_get(builder);
    webrtc_VideoFrameBuilder_set_timestamp_us(spec, webrtc_TimeMillis() * 1000);
    auto* frame = webrtc_VideoFrameBuilder_build(spec);
    webrtc_AdaptedVideoTrackSource_OnFrame(source, webrtc_VideoFrame_unique_get(frame));
    webrtc_VideoFrame_unique_delete(frame);
    webrtc_VideoFrameBuilder_unique_delete(builder);
    webrtc_VideoFrameBuffer_Release(webrtc_VideoFrameBuffer_refcounted_get(video));
    webrtc_I420Buffer_Release(planes);
}

// This source and the recording input sink are linked only to the probe executable.
class SyntheticSource final : public evp::HostMediaSource {
  public:
    SyntheticSource(Observations& o, bool audioUnavailable) : seen(o), audioUnavailable(audioUnavailable) {}
    ~SyntheticSource() override {
        stop();
    }
    bool startVideo(webrtc_AdaptedVideoTrackSource* source, int fps, Error, VideoSize size) override {
        ++seen.starts;
        seen.startingFps = fps;
        currentFps = fps;
        size(640, 360);
        video = std::jthread([source](std::stop_token stopped) {
            for (unsigned frame = 0; !stopped.stop_requested(); ++frame) {
                publish(source, frame);
                std::this_thread::sleep_for(33ms);
            }
        });
        return true;
    }
    void setVideoMaxFps(int fps) override {
        if (currentFps != fps) {
            currentFps = fps;
            seen.liveFrameRates.push_back(fps);
        }
    }
    bool startAudio(Pcm pcm, Error startupResult, Error) override {
        ++seen.audioStarts;
        audio = std::jthread([pcm = std::move(pcm), startupResult = std::move(startupResult),
                              unavailable = audioUnavailable](std::stop_token stopped) {
            if (unavailable) {
                startupResult("WASAPI default render endpoint (0x80070490)");
                return;
            }
            startupResult({});
            std::int16_t tone[960];
            std::uint64_t samples = 0;
            auto deadline = std::chrono::steady_clock::now();
            while (!stopped.stop_requested()) {
                for (int i = 0; i < 480; i++, samples++)
                    tone[i * 2] = tone[i * 2 + 1] = static_cast<std::int16_t>(
                        5000 * std::sin(2 * 3.141592653589793 * 440 * samples / 48000));
                pcm(tone, 480);
                std::this_thread::sleep_until(deadline += 10ms);
            }
        });
        return true;
    }
    void stop() override {
        if (!video.joinable() && !audio.joinable())
            return;
        video.request_stop();
        audio.request_stop();
        if (video.joinable())
            video.join();
        if (audio.joinable())
            audio.join();
        ++seen.stops;
    }
    Observations& seen;
    const bool audioUnavailable;
    int currentFps = 0;
    std::jthread video, audio;
};
void print(const QJsonObject& obj) {
    const auto b = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    std::fwrite(b.constData(), 1, b.size(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}
} // namespace

int runHostProbe(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir prefs;
    if (!prefs.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, prefs.filePath("system"));
    app.setOrganizationName("EVP-Smoke");
    app.setApplicationName("Host");
    const QUrl url(qEnvironmentVariable("EVP_PROBE_URL"));
    const QString key = qEnvironmentVariable("EVP_PROBE_KEY");
    const auto device = qEnvironmentVariable("EVP_PROBE_DEVICE_STAGE");
    const bool expires = device == "expired-pair";
    const bool autoPair = device == "auto-pair";
    const bool changedCode = device == "changed-code";
    const bool pairing = device == "pair" || autoPair || changedCode || device == "deny-pair" || device == "wrong-pair" || expires;
    const auto viewerDone = qEnvironmentVariable("EVP_PROBE_VIEWER_DONE");
    const bool revoke = device == "revoke";
    const bool disabled = device == "disabled";
    const bool locked = device == "locked";
    const bool forgotten = device == "forgotten";
    const bool disconnect = device == "disconnect";
    const bool lockActive = device == "lock-active";
    const bool sessionDisconnect = device == "session-disconnect";
    bool desktopAvailable = !locked;
    if (url.host() != "127.0.0.1" || ((pairing || device.isEmpty()) && key.isEmpty()))
        return 2;
    const auto invitationCase = qEnvironmentVariable("EVP_PROBE_CASE");
    const bool pictureSettings = invitationCase == "picture-settings";
    const bool deny = invitationCase == "deny";
    const bool legacyBroker = invitationCase == "auto-invite-legacy";
    const bool invalidProof = invitationCase == "auto-invite-invalid-proof";
    const bool autoInvite = invitationCase == "auto-invite" || legacyBroker || invalidProof;
    const bool withAudio = invitationCase != "video-only";
    const bool audioUnavailable = invitationCase == "audio-unavailable";
    const bool delayedRelease = revoke || qEnvironmentVariable("EVP_PROBE_CASE") == "release-retry";
    const bool relay = qEnvironmentVariable("EVP_PROBE_ROUTE") == "relay";
    Observations seen;
    QElapsedTimer releaseDelay;
    evp::HostAdapters adapters;
    adapters.media = [&] { return std::make_unique<SyntheticSource>(seen, audioUnavailable); };
    adapters.input = [&] {
        return std::make_unique<evp::HostInput>(
            [&](const INPUT& input) {
                if (delayedRelease && input.type == INPUT_KEYBOARD && input.ki.wScan == 48) {
                    if (!(input.ki.dwFlags & KEYEVENTF_KEYUP))
                        ++seen.heldDown;
                    else {
                        if (!releaseDelay.isValid())
                            releaseDelay.start();
                        if (releaseDelay.elapsed() < 300)
                            return false;
                        ++seen.heldUp;
                    }
                }
                if (input.type == INPUT_KEYBOARD && input.ki.wScan == 30) {
                    if (input.ki.dwFlags & KEYEVENTF_KEYUP)
                        ++seen.up;
                    else
                        ++seen.down;
                }
                if (input.type == INPUT_MOUSE) {
                    const auto f = input.mi.dwFlags;
                    if (f & MOUSEEVENTF_MOVE)
                        ++seen.move;
                    if (f & MOUSEEVENTF_WHEEL)
                        ++seen.wheel;
                    if (f & MOUSEEVENTF_LEFTDOWN)
                        ++seen.leftDown;
                    if (f & MOUSEEVENTF_LEFTUP)
                        ++seen.leftUp;
                }
                return true;
            },
            [&] {
                ++seen.armingChecks;
                return desktopAvailable ? QString{} : QStringLiteral("Test desktop unavailable");
            });
    };
    HomeWindow home;
    home.setAttribute(Qt::WA_DontShowOnScreen);
    evp::DesktopController controller(home, std::move(adapters), device_probe::storePath(prefs.path()), url);
    home.show();
    auto button = [&](const char* name) { return home.findChild<QPushButton*>(name); };
    auto* status = home.findChild<QLabel*>("bannerMessage");
    const QString preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    const device_probe::Ui ui(home);
    if ((device.isEmpty() || pairing) && !ui.saveSettings(url, key))
        return 5;
    if (!device.isEmpty()) {
        auto* inbound = home.findChild<QCheckBox*>("allowTrustedControl");
        auto* audio = home.findChild<QCheckBox*>("deviceShareAudio");
        auto* relayOnly = home.findChild<QCheckBox*>("deviceRelayOnly");
        auto* lifetime = home.findChild<QComboBox*>("pairingLifetime");
        auto* preapproved = home.findChild<QCheckBox*>("autoAcceptPairing");
        button("deviceNavigation")->click();
        if (ui.id().isEmpty())
            return 5;
        if (device == "offline-remove") {
            const auto old = ui.peer();
            ui.button("removeDevice")->click();
            const bool passed = !old.isEmpty() && ui.peer().isEmpty() && !ui.online() &&
                                ui.message().contains(QStringLiteral("已在本机移除配对"));
            print({{"passed", passed}, {"id", ui.id()}, {"previousPeer", old}});
            return passed ? 0 : 1;
        }

        if (pairing && (inbound->isChecked() || !ui.peer().isEmpty() || !lifetime || !preapproved ||
                        lifetime->currentData().toInt() != 5 || preapproved->isChecked()))
            return 5;
        if (!pairing && (ui.peer().isEmpty() != forgotten || inbound->isChecked() == disabled))
            return 5;
        if (!pairing && !disabled) {
            inbound->setChecked(true);
            audio->setChecked(withAudio);
            relayOnly->setChecked(relay);
        }

        QTimer watch;
        bool started = false, decided = false, stopping = false, disconnected = false;
        bool expired = false;
        const bool expectBlocked = disabled || locked || forgotten;
        QObject::connect(&watch, &QTimer::timeout, &app, [&] {
            if (button("hostApprove")->isVisible() || ((pairing || expectBlocked) &&
                (seen.starts || seen.audioStarts || seen.receivedInput() || (!locked && seen.armingChecks)))) {
                app.exit(5);
                return;
            }
            if (disconnect && disconnected && ui.online()) {
                const bool passed = seen.starts == 1 && seen.stops == 1 && seen.receivedInput();
                print({{"passed", passed}, {"reregistered", true}, {"id", ui.id()}, {"peer", ui.peer()},
                       {"noResumedMedia", seen.starts == 1}});
                app.exit(passed ? 0 : 1);
                return;
            }
            if (!ui.online())
                return;
            if (!started) {
                started = true;
                if (pairing) {
                    if (autoPair || changedCode || device == "wrong-pair") {
                        lifetime->setCurrentIndex(lifetime->findData(720));
                        preapproved->setChecked(true);
                    }
                    button("generatePairingCode")->click();
                    const auto code = ui.field("localPairingCode")->text();
                    if (code.isEmpty()) {
                        app.exit(5);
                        return;
                    }
                    if (changedCode) {
                        lifetime->setCurrentIndex(lifetime->findData(15));
                        if (!ui.field("localPairingCode")->text().isEmpty()) {
                            app.exit(5);
                            return;
                        }
                    }
                    print({{"pairingCode", code}});
                } else {
                    if (device == "control") {
                        if (QSystemTrayIcon::isSystemTrayAvailable()) {
                            native_ui::installSystemTray(home);
                            if (!home.close() || home.isVisible()) {
                                app.exit(5);
                                return;
                            }
                        } else {
                            home.showMinimized();
                        }
                    }
                    print({{"registered", true}, {"id", ui.id()}});
                }
            }
            if (device == "wrong-pair" || changedCode || expires) {
                if (button("approveDevicePairing")->isVisible()) {
                    app.exit(5);
                } else if (expires && !expired && ui.field("localPairingCode")->text().isEmpty()) {
                    expired = true;
                    print({{"expired", true}});
                } else if (QFileInfo::exists(viewerDone)) {
                    print({{"passed", ui.peer().isEmpty()}, {"id", ui.id()}});
                    app.exit(ui.peer().isEmpty() ? 0 : 1);
                }
                return;
            }
            if (pairing) {
                if (button("approveDevicePairing")->isVisible()) {
                    if (decided || autoPair || button("saveSettings")->isEnabled()) {
                        app.exit(5);
                        return;
                    }
                    decided = true;
                    button(device == "pair" ? "approveDevicePairing" : "denyDevicePairing")->click();
                }
                if ((!decided && !autoPair) || button("cancelDeviceAttempt")->isVisible() || !QFileInfo::exists(viewerDone))
                    return;
                const bool passed = ((device == "pair" || autoPair) == !ui.peer().isEmpty()) &&
                                    !inbound->isChecked() && !preapproved->isChecked();
                if (device == "pair")
                    inbound->setChecked(true);
                print({{"passed", passed}, {"id", ui.id()}, {"peer", ui.peer()}});
                app.exit(passed ? 0 : 1);
                return;
            }
            if (expectBlocked && QFileInfo::exists(viewerDone)) {
                const bool passed = !locked || seen.armingChecks > 0;
                print({{"passed", passed}, {"id", ui.id()}, {"peer", ui.peer()}, {"videoStarts", seen.starts}});
                app.exit(passed ? 0 : 1);
                return;
            }
            if (!stopping && seen.receivedInput() && (!delayedRelease || seen.heldDown)) {
                QPushButton* shareStop = nullptr;
                for (auto* widget : QApplication::allWidgets()) {
                    if (widget->objectName() == "trustedShareStop")
                        shareStop = qobject_cast<QPushButton*>(widget);
                }
                if (!shareStop || !shareStop->isVisible() || !shareStop->isEnabled() ||
                    !shareStop->window()->testAttribute(Qt::WA_DontShowOnScreen) || audio->isEnabled() ||
                    relayOnly->isEnabled() || !inbound->isEnabled() ||
                    button("saveSettings")->isEnabled() ||
                    (audioUnavailable && (!audio->isChecked() ||
                     !status->text().contains(QStringLiteral("系统声音不可用"))))) {
                    app.exit(5);
                    return;
                }
                stopping = true;
                if (disconnect)
                    print({{"inputDelivered", true}});
                else if (revoke)
                    inbound->setChecked(false);
                else if (lockActive)
                    desktopAvailable = false;
                else if (sessionDisconnect) {
                    DWORD sessionId = 0;
                    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) ||
                        !PostMessageW(reinterpret_cast<HWND>(home.winId()), WM_WTSSESSION_CHANGE,
                                      WTS_REMOTE_DISCONNECT, sessionId))
                        app.exit(5);
                }
                else
                    shareStop->click();
            }
        });
        QObject::connect(&controller, &evp::DesktopController::sessionFinished, &app, [&](const QString& message) {
            const bool clean = stopping && !expectBlocked && !pairing && seen.starts == 1 && seen.stops == 1 &&
                               seen.audioStarts == int(withAudio) && seen.receivedInput() && seen.startingFps == 60 &&
                               (!pictureSettings || seen.liveFrameRates == std::vector{144});
            if (!clean)
                std::fprintf(stderr, "%s\n", message.toUtf8().constData());
            if (disconnect && clean) {
                disconnected = true;
                print({{"disconnected", true}});
                return;
            }
            watch.stop();
            QTimer::singleShot(delayedRelease ? 1200 : 0, &app, [&, clean] {
                const bool passed = clean && (!delayedRelease || (seen.heldDown == 1 && seen.heldUp == 1));
                print({{"passed", passed}, {"id", ui.id()}, {"peer", ui.peer()}, {"inputDelivered", seen.receivedInput()},
                       {"videoStarts", seen.starts}, {"mediaStops", seen.stops}, {"releasedKeys", seen.heldUp}});
                app.exit(passed ? 0 : 1);
            });
        });
        watch.start(30);
        QTimer::singleShot(expires ? 350000 : 30000, &app, [&] {
            std::fprintf(stderr, "%s\n", ui.message().toUtf8().constData());
            app.exit(3);
        });
        return app.exec();
    }
    button("navigation")->click();
    if (!preview.isEmpty())
        home.grab().save(QDir(preview).filePath("unified-home.png"));
    auto* tabs = home.findChild<QTabBar*>("assistanceTabs");
    auto* invitationLifetime = home.findChild<QComboBox*>("invitationLifetime");
    auto* invitationApproval = home.findChild<QCheckBox*>("autoAcceptInvitation");
    if (!tabs || !invitationLifetime || !invitationApproval || tabs->count() != 2 ||
        invitationLifetime->currentData().toInt() != 5 || invitationApproval->isChecked())
        return 5;
    tabs->setCurrentIndex(1);
    if (autoInvite) {
        invitationLifetime->setCurrentIndex(invitationLifetime->findData(720));
        invitationApproval->setChecked(true);
        home.findChild<QCheckBox*>("hostHighRefresh")->setChecked(true);
    } else if (deny) {
        invitationLifetime->setCurrentIndex(invitationLifetime->findData(15));
    }
    if (!preview.isEmpty())
        home.grab().save(QDir(preview).filePath("unified-share.png"));
    auto* invite = home.findChild<QLineEdit*>("hostInvitation");
    bool quietAtIssue = false;
    bool inviteEmitted = false;
    QObject::connect(invite, &QLineEdit::textChanged, &app, [&](const QString& v) {
        if (!v.isEmpty()) {
            inviteEmitted = true;
            quietAtIssue = !seen.starts && !seen.audioStarts && !seen.armingChecks;
            print({{"invite", v}});
        }
    });
    bool granted = false;
    QTimer watch;
    watch.setInterval(30);
    QObject::connect(&watch, &QTimer::timeout, &app, [&] {
        if (!granted && button("hostApprove")->isVisible() && button("hostApprove")->isEnabled()) {
            if (autoInvite || seen.starts || seen.audioStarts || seen.armingChecks || seen.receivedInput()) {
                app.exit(5);
                return;
            }
            granted = true;
            auto* incoming = home.findChild<QCheckBox*>("allowTrustedControl");
            if (incoming->isEnabled()) {
                app.exit(5);
                return;
            }
            incoming->setChecked(true);
            if (incoming->isChecked()) {
                app.exit(5);
                return;
            }
            if (deny) {
                button("hostDeny")->click();
                return;
            }
            home.findChild<QCheckBox*>("hostRelayOnly")->setChecked(relay);
            home.findChild<QCheckBox*>("hostShareAudio")->setChecked(withAudio);
            home.findChild<QCheckBox*>("hostHighRefresh")->setChecked(true);
            button("hostApprove")->click();
        }
        if (seen.receivedInput() && (!delayedRelease || seen.heldDown)) {
            if (audioUnavailable && !status->text().contains(QStringLiteral("系统声音不可用"))) {
                app.exit(5);
                return;
            }
            if (autoInvite) {
                for (auto* widget : QApplication::allWidgets()) {
                    if (widget->objectName() == "trustedShareStop" && widget->isVisible()) {
                        static_cast<QPushButton*>(widget)->click();
                        return;
                    }
                }
                app.exit(5);
            } else {
                button("hostEnd")->click();
            }
        }
    });
    QObject::connect(&controller, &evp::DesktopController::sessionFinished, &app, [&](const QString& state) {
        watch.stop();
        if (legacyBroker || invalidProof) {
            const bool expected = legacyBroker ? state.contains(QStringLiteral("升级服务"))
                                              : state.contains(QStringLiteral("验证本次邀请码"));
            const bool mediaStarted = seen.starts || seen.audioStarts;
            const bool inputGranted = seen.armingChecks || seen.receivedInput();
            const bool passed = expected && inviteEmitted == invalidProof && !granted && !mediaStarted && !inputGranted;
            print({{"inviteOptionsReport", QJsonObject{{"case", invitationCase},
                   {"outcome", legacyBroker ? "upgrade-required" : "invalid-proof"}, {"inviteEmitted", inviteEmitted},
                   {"mediaStarted", mediaStarted}, {"inputGranted", inputGranted}}}});
            app.exit(passed ? 0 : 1);
            return;
        }
        const bool passed = quietAtIssue && (!pictureSettings || seen.liveFrameRates == std::vector{90}) &&
                            (deny ? (granted && !seen.starts && !seen.audioStarts && !seen.armingChecks)
                                : ((autoInvite ? !granted : granted) && seen.starts == 1 && seen.audioStarts == int(withAudio) &&
                                   seen.stops == 1 && seen.receivedInput() && seen.startingFps == 144));
        if (!passed)
            std::fprintf(stderr, "%s\n", state.toUtf8().constData());
        QTimer::singleShot(delayedRelease ? 1200 : 0, &app, [&, passed] {
            const bool cleanupOk = !delayedRelease || (seen.heldDown == 1 && seen.heldUp == 1);
            button("navigation")->click();
            tabs->setCurrentIndex(0);
            const bool canJoin = home.findChild<QLineEdit*>("invitation")->isVisible() &&
                                 button("join")->isEnabled();
            tabs->setCurrentIndex(1);
            const bool canSwitch = canJoin && button("hostCreate")->isVisible() &&
                                   invite->text().isEmpty() && !invitationApproval->isChecked();
            button("settingsNavigation")->click();
            const bool keyCleared = ui.field("settingsDeploymentKey")->isVisible() &&
                                    ui.field("settingsDeploymentKey")->text().isEmpty();
            print({{"passed", passed && cleanupOk && canSwitch && keyCleared},
                   {"roleSwitch", canSwitch},
                   {"denied", deny},
                   {"videoStarts", seen.starts},
                   {"audioStarts", seen.audioStarts},
                   {"mediaStops", seen.stops},
                   {"releasedAfterRevocation", seen.heldUp},
                   {"inputDelivered", seen.receivedInput()},
                   {"highRefreshRequested", seen.startingFps == 144}});
            app.exit(passed && cleanupOk && canSwitch && keyCleared ? 0 : 1);
        });
    });
    watch.start();
    QTimer::singleShot(0, button("hostCreate"), &QPushButton::click);
    QTimer::singleShot(65000, &app, [&] { app.exit(3); });
    return app.exec();
}
