#include "controller.h"
#include "ui/home_window.h"
#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("EVP");
    app.setApplicationName("EVP Remote");
    app.setApplicationVersion(QStringLiteral(EVP_PACKAGE_VERSION));
    app.setApplicationDisplayName(QStringLiteral("EVP 远程"));
    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::China));
    QTranslator translator;
    if (translator.load("qtbase_zh_CN", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&translator);
    HomeWindow home;
    const QSettings defaults(QCoreApplication::applicationDirPath() + "/evp-remote.ini",
                             QSettings::IniFormat);
    const auto saved = QSettings().value("signaling/url", defaults.value("signalingUrl")).toString();
    home.setSignalingUrl(QUrl(qEnvironmentVariable("EVP_SIGNAL_URL", saved)));
    evp::DesktopController controller(home);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller, &evp::DesktopController::endSession);
    home.show();
    return app.exec();
}
