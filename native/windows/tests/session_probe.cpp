#include "controller.h"
#include "media/video_surface.h"
#include "ui/home_window.h"
#include "ui/session_window.h"
#include <QApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <cstdio>

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
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir preferences;
    if (!preferences.isValid())
        return 2;
    app.setOrganizationName("EVP-Smoke");
    app.setApplicationName("NativeSession");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    InvisibleTestWindows hidden;
    app.installEventFilter(&hidden);
    HomeWindow home;
    evp::DesktopController client(home);
    home.setAttribute(Qt::WA_DontShowOnScreen);
    home.show();
    const auto url = QUrl(qEnvironmentVariable("EVP_PROBE_URL"));
    const auto invite = qEnvironmentVariable("EVP_PROBE_INVITE");
    const auto expectedRoute = qEnvironmentVariable("EVP_PROBE_ROUTE", "direct");
    if (url.host() != "127.0.0.1" || invite.isEmpty())
        return 2;
    home.setSignalingUrl(url);
    home.findChild<QLineEdit*>("invitation")->setText(invite);
    auto* join = home.findChild<QPushButton*>("join");
    bool exercised = false;
    bool ended = false;
    QObject::connect(
        &client, &evp::DesktopController::sample, &app,
        [&](quint64 presents, QSize size, QString route, bool audio, bool ready, quint64 gpu, quint64 cpu) {
            if (exercised || presents < 4 || size != QSize(640, 360) || route != expectedRoute || !audio ||
                !ready)
                return;
            SessionWindow* window = nullptr;
            for (auto* top : QApplication::topLevelWidgets())
                if ((window = qobject_cast<SessionWindow*>(top)))
                    break;
            if (!window)
                return;
            auto* surface = window->findChild<QWidget*>("remoteSurface");
            if (!surface)
                return;
            exercised = true;
            QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, surface->rect().center());
            QTest::keyClick(surface, Qt::Key_A);
            const auto position = QPointF(surface->rect().center());
            QWheelEvent wheel(position, surface->mapToGlobal(position.toPoint()), {}, QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(surface, &wheel);
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
            fprintf(stdout, "%s\n", QJsonDocument(data).toJson(QJsonDocument::Compact).constData());
            fflush(stdout);
        });
    QObject::connect(&client, &evp::DesktopController::sessionFinished, &app, [&](const QString& message) {
        ended = true;
        fprintf(stderr, "%s\n", message.toUtf8().constData());
        app.exit(exercised ? 0 : 1);
    });
    QTimer::singleShot(0, join, &QPushButton::click);
    QTimer::singleShot(60000, &app, [&] {
        if (!ended)
            app.exit(3);
    });
    return app.exec();
}
