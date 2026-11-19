#include "session_window.h"

#include "visual_style.h"

#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QShortcut>
#include <QSize>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>

namespace {

using native_ui::label;
using native_ui::Symbol;
using native_ui::symbolIcon;
using native_ui::symbolPixmap;
using native_ui::uiFont;

constexpr auto sessionStyle = R"(
    QWidget#sessionToolbar { background: #f8faff; border-bottom: 1px solid #e0e8f3; }
    QLabel { color: #182844; background: transparent; }
    QLabel#sessionMark { background: #e8f1ff; border-radius: 8px; }
    QLabel#sessionStatus, QLabel#sessionMetricName { color: #526684; }
    QFrame#toolbarDivider { background-color: #dce5f1; border: none; }
    QWidget#sessionViewport { background: #101824; }
    QPushButton#endSession {
        background: #fdf0ed; color: #963b2f; border: 1px solid #efd8d2;
        border-radius: 8px; padding: 0 12px;
    }
    QPushButton#endSession:hover { background: #fde5df; border-color: #e8b7ad; }
    QPushButton#endSession:pressed { background: #f7d7d0; }
    QPushButton#endSession:focus { border-color: #c66658; }
)";

} // namespace

SessionWindow::SessionWindow(QWidget* videoSurface, QWidget* parent)
    : QMainWindow(parent, Qt::Window)
{
    setWindowTitle(QStringLiteral("远程控制 · EVP 远程"));
    setWindowIcon(symbolIcon(Symbol::Brand, QColor("#2865d5"), 24));
    setMinimumSize(710, 380);
    resize(1100, 686);

    auto* session = new QWidget;
    session->setStyleSheet(QString::fromLatin1(sessionStyle));
    setCentralWidget(session);
    auto* layout = new QVBoxLayout(session);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget;
    toolbar_ = toolbar;
    toolbar->setObjectName(QStringLiteral("sessionToolbar"));
    toolbar->setMinimumHeight(64);
    layout->addWidget(toolbar);
    auto* tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(16, 10, 16, 10);
    tools->setSpacing(14);

    auto* identity = new QHBoxLayout;
    identity->setSpacing(10);
    auto* mark = label({}, "sessionMark", 14);
    mark->setFixedSize(34, 34);
    mark->setAlignment(Qt::AlignCenter);
    mark->setPixmap(symbolPixmap(Symbol::Display, QColor("#2b63be"), 21));
    identity->addWidget(mark, 0, Qt::AlignVCenter);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(3);
    titles->addWidget(label(QStringLiteral("远程控制"), "sessionTitle", 16, QFont::DemiBold));
    auto* status = new QHBoxLayout;
    status->setSpacing(6);
    statusIndicator_ = label({}, "sessionStatusIndicator", 12);
    statusIndicator_->setFixedSize(7, 7);
    status->addWidget(statusIndicator_, 0, Qt::AlignVCenter);
    statusLabel_ = label({}, "sessionStatus", 12);
    statusLabel_->setWordWrap(true);
    status->addWidget(statusLabel_, 1);
    titles->addLayout(status);
    identity->addLayout(titles, 1);
    tools->addLayout(identity, 1);

    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("toolbarDivider"));
    divider->setFixedSize(1, 28);
    tools->addWidget(divider, 0, Qt::AlignVCenter);

    auto* routeMetric = new QVBoxLayout;
    routeMetric->setSpacing(3);
    routeMetric->addWidget(label(QStringLiteral("连接线路"), "sessionMetricName", 12));
    routeLabel_ = label({}, "sessionRoute", 13, QFont::DemiBold);
    routeLabel_->setWordWrap(true);
    routeLabel_->setMaximumWidth(156);
    routeMetric->addWidget(routeLabel_);
    tools->addLayout(routeMetric);

    auto* videoMetric = new QVBoxLayout;
    videoMetric->setSpacing(3);
    videoMetric->addWidget(label(QStringLiteral("远程画面"), "sessionMetricName", 12));
    videoFormatLabel_ = label({}, "sessionVideoFormat", 13, QFont::DemiBold);
    videoFormatLabel_->setToolTip(QStringLiteral("远端画面尺寸及每秒成功提交给显卡的不同帧数。屏幕实际出画需另行测量。"));
    videoMetric->addWidget(videoFormatLabel_);
    tools->addLayout(videoMetric);

    inputLabel_ = label({}, "sessionInput", 12, QFont::DemiBold);
    tools->addWidget(inputLabel_, 0, Qt::AlignVCenter);
    auto* fullscreen = new QPushButton(QStringLiteral("全屏"));
    fullscreen->setObjectName(QStringLiteral("fullscreen"));
    fullscreen->setToolTip(QStringLiteral("全屏显示画面，按 Ctrl+Alt+F11 返回"));
    fullscreen->setAccessibleDescription(fullscreen->toolTip());
    fullscreen->setMinimumHeight(36);
    fullscreen->setCursor(Qt::PointingHandCursor);
    tools->addWidget(fullscreen);
    const auto toggle = [this] {
        if (isFullScreen()) showNormal();
        else showFullScreen();
    };
    connect(fullscreen, &QPushButton::clicked, this, toggle);
    auto* shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+F11")), this);
    connect(shortcut, &QShortcut::activated, this, toggle);
    auto* end = new QPushButton(QStringLiteral("结束控制"));
    end->setObjectName(QStringLiteral("endSession"));
    end->setFont(uiFont(13, QFont::DemiBold));
    end->setIcon(symbolIcon(Symbol::End, QColor("#963b2f"), 17));
    end->setIconSize(QSize(17, 17));
    end->setMinimumHeight(36);
    end->setCursor(Qt::PointingHandCursor);
    connect(end, &QPushButton::clicked, this, &SessionWindow::close);
    tools->addWidget(end, 0, Qt::AlignVCenter);

    auto* viewport = new QWidget;
    viewport->setObjectName(QStringLiteral("sessionViewport"));
    auto* viewportLayout = new QVBoxLayout(viewport);
    viewportLayout->setContentsMargins(0, 0, 0, 0);
    viewportLayout->setSpacing(0);
    videoSurface->setAccessibleName(QStringLiteral("远程画面"));
    viewportLayout->addWidget(videoSurface, 1);
    layout->addWidget(viewport, 1);

    setRoute({});
    setVideoFormat({}, 0);
    updateStatus();
}

void SessionWindow::setStatus(const QString& message)
{
    failed_ = false;
    statusMessage_ = message;
    updateStatus();
}

void SessionWindow::setConnected(bool connected)
{
    if (connected_ == connected) {
        return;
    }
    connected_ = connected;
    if (connected) {
        failed_ = false;
    } else {
        inputAvailable_ = false;
    }
    statusMessage_.clear();
    updateStatus();
}

void SessionWindow::setInputAvailable(bool available)
{
    if (inputAvailable_ == available) {
        return;
    }
    inputAvailable_ = available;
    updateStatus();
}

void SessionWindow::setRoute(const QString& route)
{
    routeLabel_->setText(route.isEmpty() ? QStringLiteral("待确认") : route);
}

void SessionWindow::setVideoFormat(const QSize& resolution, double fps)
{
    QStringList metrics;
    if (!resolution.isEmpty()) {
        metrics.append(QStringLiteral("%1 × %2").arg(resolution.width()).arg(resolution.height()));
    }
    if (std::isfinite(fps) && fps > 0) {
        const auto fractionDigits = std::floor(fps) == fps ? 0 : 1;
        metrics.append(QStringLiteral("%1 帧/秒").arg(QString::number(fps, 'f', fractionDigits)));
    }
    videoFormatLabel_->setText(metrics.isEmpty() ? QStringLiteral("等待画面")
                                                : metrics.join(QStringLiteral(" · ")));
}

void SessionWindow::showFailure(const QString& message)
{
    failed_ = true;
    connected_ = false;
    inputAvailable_ = false;
    statusMessage_ = message;
    updateStatus();
}

void SessionWindow::finish()
{
    closeHandled_ = true;
    close();
}

void SessionWindow::closeEvent(QCloseEvent* event)
{
    QMainWindow::closeEvent(event);
    if (!closeHandled_ && event->isAccepted()) {
        closeHandled_ = true;
        emit endRequested();
    }
}

void SessionWindow::updateStatus()
{
    if (failed_) {
        statusIndicator_->setStyleSheet(QStringLiteral("background: #bb4c3b; border-radius: 3px;"));
        statusLabel_->setText(statusMessage_.isEmpty() ? QStringLiteral("连接已中断") : statusMessage_);
    } else if (connected_) {
        statusIndicator_->setStyleSheet(QStringLiteral("background: #268568; border-radius: 3px;"));
        statusLabel_->setText(statusMessage_.isEmpty() ? QStringLiteral("已连接") : statusMessage_);
    } else {
        statusIndicator_->setStyleSheet(QStringLiteral("background: #b67b23; border-radius: 3px;"));
        statusLabel_->setText(statusMessage_.isEmpty() ? QStringLiteral("正在建立连接") : statusMessage_);
    }

    inputLabel_->setVisible(connected_ && !failed_);
    if (inputAvailable_) {
        inputLabel_->setText(QStringLiteral("可控制"));
        inputLabel_->setStyleSheet(QStringLiteral(
            "background: #e3f4ed; color: #20694f; border-radius: 11px; padding: 4px 9px;"));
    } else {
        inputLabel_->setText(QStringLiteral("仅查看"));
        inputLabel_->setStyleSheet(QStringLiteral(
            "background: #eaf0fb; color: #435b81; border-radius: 11px; padding: 4px 9px;"));
    }
}

void SessionWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && toolbar_)
        toolbar_->setVisible(!isFullScreen());
}
