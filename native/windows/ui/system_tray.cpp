#include "system_tray.h"

#include "home_window.h"

#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>

namespace native_ui {

void installSystemTray(HomeWindow& home) {
    auto* tray = new QSystemTrayIcon(home.windowIcon(), &home);
    tray->setToolTip(QApplication::applicationDisplayName());

    auto* menu = new QMenu(&home);
    auto* open = menu->addAction(QStringLiteral("打开"));
    open->setObjectName("trayOpen");
    menu->setDefaultAction(open);
    auto* quit = menu->addAction(QStringLiteral("退出"));
    quit->setObjectName("trayQuit");
    tray->setContextMenu(menu);

    const auto showHome = [&home] {
        if (home.isMinimized())
            home.setWindowState(home.windowState() & ~Qt::WindowMinimized);
        home.show();
        home.showPendingConfirmation();
        home.raise();
        home.activateWindow();
    };
    QObject::connect(open, &QAction::triggered, tray, showHome);
    QObject::connect(tray, &QSystemTrayIcon::activated, &home, [showHome](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            showHome();
    });
    QObject::connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
    QObject::connect(&home, &HomeWindow::backgroundConfirmation, tray, [tray, showHome](const QString& message) {
        if (QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages())
            tray->showMessage(tray->toolTip(), message);
        else
            showHome();
    });
    QObject::connect(tray, &QSystemTrayIcon::messageClicked, &home, showHome);

    // Let windows close normally so the updater and explicit Quit can still shut down.
    QApplication::setQuitOnLastWindowClosed(false);
    QObject::connect(qApp, &QApplication::lastWindowClosed, tray, [] {
        if (!QSystemTrayIcon::isSystemTrayAvailable())
            QCoreApplication::quit();
    });
    tray->show();
}

}
