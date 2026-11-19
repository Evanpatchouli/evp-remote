#include "controller.h"
#include "ui/focus_style.h"
#include "ui/home_window.h"
#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("EVP");
    app.setApplicationName("EVP Remote");
    app.setApplicationVersion(QStringLiteral(EVP_PACKAGE_VERSION));
    app.setApplicationDisplayName(QStringLiteral("EVP 远程"));
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts(EVP_PACKAGE_VERSION);
        return 0;
    }
    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::China));
    QTranslator translator;
    if (translator.load("qtbase_zh_CN", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&translator);
    native_ui::installFocusStyle(app);
    HomeWindow home;
    const QSettings defaults(QCoreApplication::applicationDirPath() + "/evp-remote.ini",
                             QSettings::IniFormat);
    const auto address = qEnvironmentVariable("EVP_SIGNAL_URL", defaults.value("signalingUrl").toString());
    evp::DesktopController controller(home, {}, {}, QUrl(address, QUrl::StrictMode));
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller, &evp::DesktopController::endSession);
    home.show();
    return app.exec();
}
