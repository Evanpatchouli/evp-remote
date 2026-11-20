#include "controller.h"
#include "device_probe.h"
#include "media/video_surface.h"
#include "ui/home_window.h"
#include "ui/session_window.h"
#include <QApplication>
#include <QCheckBox>
#include <QAbstractButton>
#include <QLabel>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <cstdio>
#include <windows.h>

namespace {
class InvisibleTestWindows final : public QObject {
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Polish)
            if (auto* window = qobject_cast<QMainWindow*>(object))
                window->setAttribute(Qt::WA_DontShowOnScreen);
        return false;
    }
};
} // namespace

// Separate test binary: drive the actual Qt form, session and input path without OS-level input.
int runViewerProbe(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir preferences;
    if (!preferences.isValid())
        return 2;
    app.setOrganizationName("EVP-Smoke");
    app.setApplicationName("NativeSession");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, preferences.filePath("system"));
    InvisibleTestWindows hidden;
    const auto screenshot = qEnvironmentVariable("EVP_PROBE_SCREENSHOT");
    if (screenshot.isEmpty())
        app.installEventFilter(&hidden);
    HomeWindow home;
    home.setAttribute(Qt::WA_DontShowOnScreen);
    const auto url = QUrl(qEnvironmentVariable("EVP_PROBE_URL"));
    const auto invite = qEnvironmentVariable("EVP_PROBE_INVITE");
    const auto device = qEnvironmentVariable("EVP_PROBE_DEVICE_STAGE");
    const auto pairingCode = qEnvironmentVariable("EVP_PROBE_PAIRING_CODE");
    const bool willPair = device == "pair" || device == "auto-pair";
    const bool pairing = willPair || device == "changed-code" || device == "deny-pair" ||
                         device == "wrong-pair" || device == "expired-pair";
    const bool expectBlocked = device == "disabled" || device == "locked" || device == "forgotten";
    const auto expectedRoute = qEnvironmentVariable("EVP_PROBE_ROUTE", "direct");
    const auto scenario = qEnvironmentVariable("EVP_PROBE_CASE");
    const bool pictureSettings = scenario == "picture-settings";
    const bool expectDenial = scenario == "deny";
    const bool expectAudio = scenario != "video-only" && scenario != "audio-unavailable";
    if (url.host() != "127.0.0.1" || (device.isEmpty() && invite.isEmpty()) || (pairing && pairingCode.isEmpty()))
        return 2;
    evp::DesktopController client(home, {}, device_probe::storePath(preferences.path()), url);
    home.show();
    const device_probe::Ui deviceUi(home);
    if (device.isEmpty()) {
        if (!deviceUi.saveSettings(url))
            return 5;
        deviceUi.button("navigation")->click();
    } else {
        const auto key = qEnvironmentVariable("EVP_PROBE_KEY");
        if (pairing && (key.isEmpty() || !deviceUi.saveSettings(url, key)))
            return 5;
        deviceUi.button("deviceNavigation")->click();
    }
    home.findChild<QLineEdit*>("invitation")->setText(invite);
    auto* join = home.findChild<QPushButton*>("join");
    bool requested = false;
    bool retriedCode = false;
    QTimer watch;
    if (!device.isEmpty()) {
        const auto expectedPeer = qEnvironmentVariable("EVP_PROBE_TARGET");
        if (deviceUi.id().isEmpty() || (pairing ? !deviceUi.peer().isEmpty() : deviceUi.peer() != expectedPeer))
            return 5;
        QObject::connect(&watch, &QTimer::timeout, &app, [&, expectedPeer] {
            if (pairing || expectBlocked) {
                for (auto* top : QApplication::topLevelWidgets()) {
                    if (qobject_cast<SessionWindow*>(top)) {
                        app.exit(5);
                        return;
                    }
                }
            }
            if (!deviceUi.online())
                return;
            if (!requested) {
                if (pairing) {
                    requested = true;
                    deviceUi.field("peerPairingCode")->setText(pairingCode);
                    deviceUi.button("pairDevice")->click();
                } else if (deviceUi.button("connectDevice")->isEnabled()) {
                    requested = true;
                    deviceUi.button("connectDevice")->click();
                }
                return;
            }
            if ((!pairing && !expectBlocked) || deviceUi.button("cancelDeviceAttempt")->isVisible())
                return;
            const bool passed = willPair ? deviceUi.peer() == expectedPeer
                : deviceUi.message().contains(QStringLiteral("对方未授权或已拒绝")) &&
                  (pairing ? deviceUi.peer().isEmpty() : deviceUi.peer() == expectedPeer);
            if (passed && device == "deny-pair" && !retriedCode) {
                retriedCode = true;
                requested = false;
                return;
            }
            if (device == "pair" && passed)
                home.findChild<QCheckBox*>("allowTrustedControl")->setChecked(true);
            device_probe::print({{"passed", passed}, {"id", deviceUi.id()}, {"peer", deviceUi.peer()}});
            if (!passed)
                std::fprintf(stderr, "%s\n", deviceUi.message().toUtf8().constData());
            app.exit(passed ? 0 : 1);
        });
        watch.start(30);
    }
    bool exercised = false;
    bool ended = false;
    int pictureStep = 0;
    QObject::connect(
        &client, &evp::DesktopController::sample, &app,
        [&](quint64 presents, QSize size, QString route, bool audio, bool ready, quint64 gpu, quint64 cpu) {
            if (exercised || presents < 4 || size != QSize(640, 360) || route != expectedRoute ||
                audio != expectAudio || !ready)
                return;
            if (expectDenial) {
                app.exit(4);
                return;
            }
            SessionWindow* window = nullptr;
            for (auto* top : QApplication::topLevelWidgets())
                if ((window = qobject_cast<SessionWindow*>(top)))
                    break;
            if (!window)
                return;
            if (!expectAudio &&
                !window->findChild<QLabel*>("sessionStatus")->text().contains(QStringLiteral("对方未共享声音"))) {
                app.exit(5);
                return;
            }
            auto* surface = window->findChild<QWidget*>("remoteSurface");
            if (!surface)
                return;
            if (pictureSettings) {
                auto* trigger = window->findChild<QAbstractButton*>("sessionVideoSettings");
                auto* panel = window->findChild<QWidget*>("videoSettingsPanel");
                auto* hd = window->findChild<QAbstractButton*>("videoBitrate8");
                auto* initialFps = window->findChild<QAbstractButton*>(device.isEmpty() ? "videoFps144" : "videoFps60");
                auto* targetFps = window->findChild<QAbstractButton*>(device.isEmpty() ? "videoFps90" : "videoFps144");
                auto* automatic = window->findChild<QAbstractButton*>("videoBitrateAuto");
                auto* custom = window->findChild<QSpinBox*>("videoCustomBitrate");
                auto* apply = window->findChild<QAbstractButton*>("videoCustomApply");
                if (!trigger || !panel || !hd || !initialFps || !targetFps || !automatic || !custom || !apply) {
                    app.exit(5);
                    return;
                }
                if (pictureStep == 0) {
                    if (screenshot.isEmpty())
                        panel->setAttribute(Qt::WA_DontShowOnScreen);
                    else if (!SetWindowPos(reinterpret_cast<HWND>(window->winId()), HWND_TOPMOST, 0, 0, 0, 0,
                                           SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
                        app.exit(5);
                        return;
                    }
                    trigger->click();
                    if (!panel->isVisible()) {
                        app.exit(5);
                        return;
                    }
                    ++pictureStep;
                } else if (pictureStep == 1 && hd->isEnabled() && automatic->isChecked()) {
                    if (!initialFps->isChecked()) {
                        app.exit(5);
                        return;
                    }
                    hd->click();
                    ++pictureStep;
                } else if (pictureStep == 2 && hd->isEnabled() && hd->isChecked()) {
                    targetFps->click();
                    ++pictureStep;
                } else if (pictureStep == 3 && targetFps->isEnabled() && targetFps->isChecked()) {
                    if (!screenshot.isEmpty()) {
                        const auto bounds = window->frameGeometry().united(panel->frameGeometry());
                        const auto offset = bounds.topLeft() - window->screen()->geometry().topLeft();
                        if (!window->screen()->grabWindow(0, offset.x(), offset.y(), bounds.width(), bounds.height())
                                 .save(screenshot)) {
                            app.exit(5);
                            return;
                        }
                    }
                    custom->setValue(22);
                    apply->click();
                    ++pictureStep;
                } else if (pictureStep == 4 && automatic->isEnabled() && !hd->isChecked() &&
                           !automatic->isChecked() && custom->value() == 22) {
                    automatic->click();
                    ++pictureStep;
                } else if (pictureStep == 5 && automatic->isEnabled() && automatic->isChecked() && targetFps->isChecked()) {
                    panel->hide();
                    ++pictureStep;
                }
                if (pictureStep != 6)
                    return;
            }
            exercised = true;
            QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, surface->rect().center());
            QTest::keyClick(surface, Qt::Key_A);
            const auto position = QPointF(surface->rect().center());
            QWheelEvent wheel(position, surface->mapToGlobal(position.toPoint()), {}, QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(surface, &wheel);
            if (device == "revoke" || qEnvironmentVariable("EVP_PROBE_CASE") == "release-retry")
                QTest::keyPress(surface, Qt::Key_B);
            const auto* renderer = static_cast<evp::VideoSurface*>(surface);
            const auto data =
                QJsonObject{{"presents", static_cast<qint64>(presents)},
                            {"route", route},
                            {"receivedAudio", audio},
                            {"sourceWidth", size.width()},
                            {"sourceHeight", size.height()},
                            {"gpuDecoded", static_cast<qint64>(gpu)},
                            {"cpuDecoded", static_cast<qint64>(cpu)},
                            {"presentationFailures", static_cast<qint64>(renderer->stats().failures)}};
            auto report = data;
            if (pictureSettings)
                report.insert("pictureSettingsApplied", pictureStep == 6);
            if (!device.isEmpty()) {
                report.insert("id", deviceUi.id());
                report.insert("peer", deviceUi.peer());
            }
            device_probe::print(report);
        });
    QObject::connect(&client, &evp::DesktopController::sessionFinished, &app, [&](const QString& message) {
        ended = true;
        fprintf(stderr, "%s\n", message.toUtf8().constData());
        app.exit((expectDenial ? (!exercised && message.contains(QStringLiteral("拒绝"))) : exercised) ? 0
                                                                                                       : 1);
    });
    if (device.isEmpty())
        QTimer::singleShot(0, join, &QPushButton::click);
    QTimer::singleShot(device.isEmpty() ? 60000 : 28000, &app, [&] {
        if (!ended) {
            if (!device.isEmpty())
                std::fprintf(stderr, "%s\n", deviceUi.message().toUtf8().constData());
            app.exit(3);
        }
    });
    return app.exec();
}

#ifndef EVP_UNIFIED_PROBE
int main(int argc, char** argv) {
    return runViewerProbe(argc, argv);
}
#endif
