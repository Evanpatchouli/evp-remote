#include "session_window.h"

#include "action_button.h"
#include "floating_surface.h"
#include "visual_style.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QScreen>
#include <QShortcut>
#include <QSize>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>

namespace {

using native_ui::ActionButton;
using native_ui::label;
using native_ui::Symbol;
using native_ui::symbolIcon;
using native_ui::symbolPixmap;
using native_ui::uiFont;

constexpr auto sessionStyle = R"(
    QWidget#sessionToolbar { background: #f8faff; border-bottom: 1px solid #e0e8f3; }
    QLabel { color: #182844; background: transparent; }
    QLabel#sessionMark { background: #e8f1ff; border-radius: 8px; }
    QLabel#sessionStatus { color: #526684; }
    QFrame#toolbarDivider { background-color: #dce5f1; border: none; }
    QWidget#sessionViewport { background: #101824; }
    QPushButton#sessionVideoSettings {
        background: #f4f8ff; color: #182844; border: 1px solid #dbe5f5; padding: 0 10px;
    }
    QPushButton#sessionVideoSettings:hover { background: #e5eeff; border-color: #b2c8eb; }
    QPushButton#sessionVideoSettings:pressed { background: #d9e7ff; }
    QPushButton#sessionVideoSettings:focus { border-color: #3475e4; }
    QPushButton#fullscreen {
        background: palette(button); color: palette(button-text); border: 1px solid palette(mid); padding: 0 12px;
    }
    QPushButton#fullscreen:hover { background: palette(light); }
    QPushButton#fullscreen:pressed { background: palette(midlight); }
    QPushButton#fullscreen:focus { border-color: palette(highlight); }
    QPushButton#endSession {
        background: #fdf0ed; color: #963b2f; border: 1px solid #efd8d2; padding: 0 12px;
    }
    QPushButton#endSession:hover { background: #fde5df; border-color: #e8b7ad; }
    QPushButton#endSession:pressed { background: #f7d7d0; }
    QPushButton#endSession:focus { border-color: #c66658; }
)";

constexpr auto videoSettingsStyle = R"(
    QFrame#videoSettingsSurface {
        background: #ffffff; border: 1px solid #bdcde4; border-radius: 10px;
    }
    QFrame#videoSettingsDivider { background: #e0e8f3; border: none; }
    QLabel { color: #182844; background: transparent; }
    QLabel#videoBitrateRange, QLabel#videoColorNote { color: #526684; }
    QLabel#videoSettingsMessage {
        background: #f1f5fd; color: #435b81; border-radius: 6px; padding: 6px 8px;
    }
    QPushButton[videoChoice="true"], QPushButton#videoCustomApply {
        background: #f8faff; color: #263955; border: 1px solid #d9e3f1;
        padding: 0 8px;
    }
    QPushButton[videoChoice="true"]:hover, QPushButton#videoCustomApply:hover {
        background: #edf4ff; border-color: #b2c8eb;
    }
    QPushButton[videoChoice="true"]:checked, QPushButton#videoCustomApply {
        background: #e8f1ff; color: #214c98; border-color: #9ebbee;
    }
    QPushButton[videoChoice="true"]:focus, QPushButton#videoCustomApply:focus {
        border-color: #3475e4;
    }
    QPushButton[videoChoice="true"]:pressed, QPushButton#videoCustomApply:pressed {
        background: #d9e7ff;
    }
    QPushButton[videoChoice="true"]:disabled, QPushButton#videoCustomApply:disabled {
        background: #f4f7fc; color: #667893; border-color: #e0e8f3;
    }
    QPushButton[videoChoice="true"]:checked:disabled {
        background: #edf3fd; color: #435b81; border-color: #abbedb;
    }
    QSpinBox#videoCustomBitrate {
        background: #ffffff; color: #182844; border: 1px solid #cbd7e8;
        border-radius: 7px; padding: 0 8px;
    }
    QSpinBox#videoCustomBitrate:focus { border-color: #3475e4; }
    QSpinBox#videoCustomBitrate:disabled { background: #f4f7fc; color: #667893; }
)";

struct VideoOption {
    int limit;
    const char* name;
    QString text;
};

class VideoChoice final : public ActionButton {
public:
    explicit VideoChoice(const VideoOption& option) : ActionButton(option.text) {
        setObjectName(QString::fromLatin1(option.name));
        setProperty("videoChoice", true);
        setFont(uiFont(13, QFont::DemiBold));
        setCheckable(true);
    }

protected:
    // The peer confirms settings before any option becomes selected.
    void nextCheckState() override {}
};

QPushButton* addVideoChoice(QHBoxLayout* row, const VideoOption& option) {
    auto* button = new VideoChoice(option);
    row->addWidget(button, 1);
    return button;
}

void addSettingsDivider(QVBoxLayout* layout) {
    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("videoSettingsDivider"));
    divider->setFixedHeight(1);
    layout->addWidget(divider);
}

} // namespace

SessionWindow::SessionWindow(QWidget* videoSurface, QWidget* parent)
    : QMainWindow(parent, Qt::Window)
{
    setWindowTitle(QStringLiteral("远程控制 · EVP 远程"));
    setWindowIcon(native_ui::brandIcon());
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

    routeLabel_ = label({}, "sessionRoute", 13, QFont::DemiBold);
    routeLabel_->setWordWrap(true);
    routeLabel_->setMaximumWidth(156);
    tools->addWidget(routeLabel_, 0, Qt::AlignVCenter);

    videoSettingsButton_ = new ActionButton(symbolIcon(Symbol::ChevronDown, QColor("#456594"), 14), {});
    videoSettingsButton_->setObjectName(QStringLiteral("sessionVideoSettings"));
    videoSettingsButton_->setAccessibleName(QStringLiteral("远程画面设置"));
    videoSettingsButton_->setToolTip(QStringLiteral("点击调整画面设置。按钮显示远端画面尺寸和每秒提交给显卡的不同帧数。"));
    videoSettingsButton_->setFont(uiFont(13, QFont::DemiBold));
    videoSettingsButton_->setIconSize(QSize(14, 14));
    videoSettingsButton_->setLayoutDirection(Qt::RightToLeft);
    tools->addWidget(videoSettingsButton_, 0, Qt::AlignVCenter);
    createVideoSettingsPanel();
    connect(videoSettingsButton_, &QPushButton::clicked, this, [this] {
        videoSettingsFocus_ = nullptr;
        positionVideoSettingsPanel();
        videoSettingsPanel_->show();
        if (videoSettingsAvailable_) {
            auto* selected = bitrateOptions_->button(maxVideoBitrateMbps_);
            QWidget* focus = selected ? static_cast<QWidget*>(selected) : customBitrate_;
            focus->setFocus(Qt::PopupFocusReason);
        } else {
            videoSettingsPanel_->setFocus(Qt::PopupFocusReason);
        }
    });

    inputLabel_ = label({}, "sessionInput", 12, QFont::DemiBold);
    tools->addWidget(inputLabel_, 0, Qt::AlignVCenter);
    auto* fullscreen = new ActionButton(QStringLiteral("全屏"));
    fullscreen->setObjectName(QStringLiteral("fullscreen"));
    fullscreen->setToolTip(QStringLiteral("全屏显示画面，按 Ctrl+Alt+F11 返回"));
    fullscreen->setAccessibleDescription(fullscreen->toolTip());
    tools->addWidget(fullscreen);
    const auto toggle = [this] {
        videoSettingsPanel_->hide();
        if (isFullScreen()) showNormal();
        else showFullScreen();
    };
    connect(fullscreen, &QPushButton::clicked, this, toggle);
    auto* shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+F11")), this);
    connect(shortcut, &QShortcut::activated, this, toggle);
    auto* popupShortcut = new QShortcut(shortcut->key(), videoSettingsPanel_);
    connect(popupShortcut, &QShortcut::activated, this, toggle);
    auto* end = new ActionButton(symbolIcon(Symbol::Power, QColor("#963b2f"), 17), QStringLiteral("结束控制"));
    end->setObjectName(QStringLiteral("endSession"));
    end->setFont(uiFont(13, QFont::DemiBold));
    end->setIconSize(QSize(17, 17));
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
    setVideoSettings(0, 0, false, QStringLiteral("等待远端画面设置可用"));
    updateStatus();
}

void SessionWindow::createVideoSettingsPanel()
{
    videoSettingsPanel_ = new QDialog(this, Qt::Popup);
    videoSettingsPanel_->setObjectName(QStringLiteral("videoSettingsPanel"));
    videoSettingsPanel_->setAccessibleName(QStringLiteral("画面设置"));
    videoSettingsPanel_->setFocusPolicy(Qt::StrongFocus);
    videoSettingsPanel_->setStyleSheet(QString::fromLatin1(videoSettingsStyle));

    auto* surface = new QFrame(videoSettingsPanel_);
    surface->setObjectName(QStringLiteral("videoSettingsSurface"));
    surface->setFixedWidth(408);
    native_ui::styleFloatingSurface(*videoSettingsPanel_, *surface);
    auto* popupLayout = new QVBoxLayout(videoSettingsPanel_);
    popupLayout->setContentsMargins(0, 0, 0, 0);
    popupLayout->addWidget(surface);

    auto* layout = new QVBoxLayout(surface);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    layout->addWidget(label(QStringLiteral("画面设置"), "videoSettingsTitle", 17, QFont::DemiBold));

    layout->addWidget(label(QStringLiteral("画质"), "videoQualityHeading", 13, QFont::DemiBold));

    auto* quality = new QHBoxLayout;
    quality->setSpacing(6);
    bitrateOptions_ = new QButtonGroup(this);
    bitrateOptions_->setExclusive(false);
    const VideoOption qualityOptions[] = {
        {0, "videoBitrateAuto", QStringLiteral("自动（原画）")},
        {30, "videoBitrate30", QStringLiteral("原画 30M")},
        {14, "videoBitrate14", QStringLiteral("超清 14M")},
        {8, "videoBitrate8", QStringLiteral("高清 8M")},
    };
    for (const auto& option : qualityOptions)
        bitrateOptions_->addButton(addVideoChoice(quality, option), option.limit);
    layout->addLayout(quality);
    connect(bitrateOptions_, &QButtonGroup::idClicked, this, [this](int limit) {
        requestVideoSettings(limit, maxVideoFps_);
    });

    auto* custom = new QHBoxLayout;
    custom->setSpacing(8);
    auto* customLabel = label(QStringLiteral("自定义"), "videoCustomLabel", 13);
    custom->addWidget(customLabel);
    customBitrateInfo_ = label({}, "videoBitrateRange", 12);
    custom->addWidget(customBitrateInfo_);
    custom->addStretch();
    customBitrate_ = new QSpinBox;
    customBitrate_->setObjectName(QStringLiteral("videoCustomBitrate"));
    customBitrate_->setAccessibleName(QStringLiteral("自定义码率上限"));
    customBitrate_->setFont(uiFont(13));
    customBitrate_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    customBitrate_->setRange(1, 50);
    customBitrate_->setValue(20);
    customBitrate_->setSuffix(QStringLiteral(" Mbps"));
    customBitrate_->setFixedWidth(106);
    customBitrate_->setMinimumHeight(32);
    customLabel->setBuddy(customBitrate_);
    custom->addWidget(customBitrate_);
    applyBitrate_ = new ActionButton(QStringLiteral("应用"));
    applyBitrate_->setObjectName(QStringLiteral("videoCustomApply"));
    applyBitrate_->setAccessibleName(QStringLiteral("应用自定义码率"));
    applyBitrate_->setFont(uiFont(13, QFont::DemiBold));
    applyBitrate_->setMinimumWidth(54);
    custom->addWidget(applyBitrate_);
    connect(applyBitrate_, &QPushButton::clicked, this, [this] {
        customBitrate_->interpretText();
        requestVideoSettings(customBitrate_->value(), maxVideoFps_);
    });
    layout->addLayout(custom);

    addSettingsDivider(layout);
    layout->addWidget(label(QStringLiteral("帧率上限"), "videoFpsHeading", 13, QFont::DemiBold));
    auto* frameRate = new QHBoxLayout;
    frameRate->setSpacing(6);
    frameRateOptions_ = new QButtonGroup(this);
    frameRateOptions_->setExclusive(false);
    const VideoOption frameRateOptions[] = {
        {144, "videoFps144", QStringLiteral("144 帧")},
        {90, "videoFps90", QStringLiteral("90 帧")},
        {60, "videoFps60", QStringLiteral("60 帧")},
        {30, "videoFps30", QStringLiteral("30 帧")},
    };
    for (const auto& option : frameRateOptions)
        frameRateOptions_->addButton(addVideoChoice(frameRate, option), option.limit);
    layout->addLayout(frameRate);
    connect(frameRateOptions_, &QButtonGroup::idClicked, this, [this](int limit) {
        requestVideoSettings(maxVideoBitrateMbps_, limit);
    });

    addSettingsDivider(layout);
    layout->addWidget(label(QStringLiteral("色彩"), "videoColorHeading", 13, QFont::DemiBold));
    auto* color = new QHBoxLayout;
    color->setSpacing(6);
    const auto colorNote = QStringLiteral("当前会话使用 8 位 4:2:0 SDR，暂不支持真彩或 HDR。");
    auto* trueColor = addVideoChoice(color, {0, "videoColorTrue", QStringLiteral("真彩")});
    trueColor->setEnabled(false);
    trueColor->setAccessibleDescription(colorNote);
    auto* sdr = addVideoChoice(color, {0, "videoColorSdr", QStringLiteral("常规色彩")});
    sdr->setChecked(true);
    sdr->setFocusPolicy(Qt::NoFocus);
    sdr->unsetCursor();
    auto* hdr = addVideoChoice(color, {0, "videoColorHdr", QStringLiteral("HDR")});
    hdr->setEnabled(false);
    hdr->setAccessibleDescription(colorNote);
    layout->addLayout(color);
    layout->addWidget(label(colorNote, "videoColorNote", 12));

    videoSettingsMessage_ = label({}, "videoSettingsMessage", 12);
    videoSettingsMessage_->setWordWrap(true);
    layout->addWidget(videoSettingsMessage_);

    auto* applyShortcut = new QShortcut(customBitrate_);
    applyShortcut->setKeys({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    applyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(applyShortcut, &QShortcut::activated, applyBitrate_, &QPushButton::click);
}

void SessionWindow::positionVideoSettingsPanel()
{
    videoSettingsPanel_->layout()->activate();
    videoSettingsPanel_->resize(videoSettingsPanel_->sizeHint());
    const QRect available = videoSettingsButton_->screen()->availableGeometry();
    const auto margins = videoSettingsPanel_->contentsMargins();
    const auto anchor = videoSettingsButton_->mapToGlobal(QPoint(videoSettingsButton_->width(), 0));
    const int width = videoSettingsPanel_->width();
    const int height = videoSettingsPanel_->height();
    const int left = anchor.x() + margins.right() - width;
    const int below = anchor.y() + videoSettingsButton_->height() + 6 - margins.top();
    const int above = anchor.y() - 6 - height + margins.bottom();
    const int top = below + height <= available.bottom() + 1 ? below : above;
    videoSettingsPanel_->move(qBound(available.left(), left, qMax(available.left(), available.right() + 1 - width)),
                              qBound(available.top(), top, qMax(available.top(), available.bottom() + 1 - height)));
}

void SessionWindow::requestVideoSettings(int maxBitrateMbps, int maxFps)
{
    if (videoSettingsAvailable_ && (maxBitrateMbps != maxVideoBitrateMbps_ || maxFps != maxVideoFps_))
        emit videoSettingsRequested(maxBitrateMbps, maxFps);
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
    routeLabel_->setAccessibleName(QStringLiteral("连接线路：%1").arg(routeLabel_->text()));
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
    const auto format = metrics.isEmpty() ? QStringLiteral("等待画面") : metrics.join(QStringLiteral(" · "));
    videoSettingsButton_->setText(format);
    videoSettingsButton_->setAccessibleDescription(format + QStringLiteral("。打开画面设置"));
}

void SessionWindow::setVideoSettings(int maxBitrateMbps, int maxFps, bool available, const QString& message)
{
    if (videoSettingsAvailable_ && !available && videoSettingsPanel_->isVisible())
        videoSettingsFocus_ = videoSettingsPanel_->focusWidget();
    const bool custom = maxBitrateMbps > 0 && !bitrateOptions_->button(maxBitrateMbps);
    if (custom && maxVideoBitrateMbps_ != maxBitrateMbps)
        customBitrate_->setValue(maxBitrateMbps);

    maxVideoBitrateMbps_ = maxBitrateMbps;
    maxVideoFps_ = maxFps;
    videoSettingsAvailable_ = available;

    const auto updateOptions = [available](const QButtonGroup& group, int limit) {
        for (auto* button : group.buttons()) {
            button->setChecked(group.id(button) == limit);
            button->setEnabled(available);
        }
    };
    updateOptions(*bitrateOptions_, maxBitrateMbps);
    updateOptions(*frameRateOptions_, maxFps);
    customBitrate_->setEnabled(available);
    applyBitrate_->setEnabled(available);
    customBitrateInfo_->setText(custom ? QStringLiteral("当前 %1 Mbps").arg(maxBitrateMbps)
                                     : QStringLiteral("1–50 Mbps"));

    videoSettingsMessage_->setText(message);
    videoSettingsMessage_->setVisible(!message.isEmpty());
    if (videoSettingsPanel_->isVisible())
        positionVideoSettingsPanel();
    if (available && videoSettingsFocus_) {
        if (videoSettingsPanel_->isActiveWindow())
            videoSettingsFocus_->setFocus(Qt::PopupFocusReason);
        videoSettingsFocus_ = nullptr;
    }
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
    videoSettingsPanel_->hide();
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
    if (event->type() == QEvent::WindowStateChange && toolbar_) {
        if (isFullScreen() || isMinimized())
            videoSettingsPanel_->hide();
        toolbar_->setVisible(!isFullScreen());
    }
}
