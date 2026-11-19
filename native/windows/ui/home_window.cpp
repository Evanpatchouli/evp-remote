#include "home_window.h"

#include "device_page.h"
#include "visual_style.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSysInfo>
#include <QVBoxLayout>

#include <Windows.h>
#include <wtsapi32.h>
#include <limits>
#include <utility>

namespace {

using native_ui::label;
using native_ui::Symbol;
using native_ui::symbolIcon;
using native_ui::symbolPixmap;
using native_ui::uiFont;

const auto idleMessage = QStringLiteral("输入对方发来的邀请码，在对方确认后开始控制。");
const auto waitingMessage = QStringLiteral("正在向对方发送连接请求。");
const auto hostIdleMessage = QStringLiteral("创建一次性邀请码，收到请求后由你决定是否共享本机。");

constexpr auto homeStyle = R"(
    QWidget#home, QWidget#homeBody, QScrollArea#homeScroll { background: #f7f9fd; }
    QWidget#rail { background: #eaf1fc; border-right: 1px solid #e1eafa; }
    QLabel { color: #182844; background: transparent; }
    QLabel#subtle, QLabel#machinePlatform { color: #566986; }
    QLabel#brandMark { background: #2865d5; border-radius: 10px; }
    QLabel#deviceIcon { background: #ecf3ff; border-radius: 10px; }
    QFrame#card { background: #ffffff; border: 1px solid #e0e8f3; border-radius: 14px; }
    QLineEdit {
        background: #ffffff; color: #182844; selection-background-color: #2364da;
        selection-color: #ffffff; border: 1px solid #cbd7e8; border-radius: 8px; padding: 0 12px;
    }
    QLineEdit:hover { border-color: #9fb7d8; }
    QLineEdit:focus { border: 2px solid #3475e4; padding: 0 11px; }
    QLineEdit:disabled { background: #f4f7fc; color: #566986; }
    QPushButton[roleNavigation="true"] {
        background: transparent; color: #214c98; border: 1px solid transparent;
        border-radius: 9px; padding: 0 12px; text-align: left;
    }
    QPushButton[roleNavigation="true"]:checked { background: #dbe8ff; }
    QPushButton[roleNavigation="true"]:hover { background: #cddfff; }
    QPushButton[roleNavigation="true"]:pressed { background: #c2d6fa; }
    QPushButton[roleNavigation="true"]:focus { border-color: #3475e4; }
    QPushButton[roleNavigation="true"]:disabled { color: #4a6592; }
    QPushButton#paste, QPushButton#cancel, QPushButton#copyHostInvitation, QPushButton#hostDeny {
        background: #f8faff; color: #284675; border: 1px solid #ccd9ec;
        border-radius: 8px; padding: 0 12px;
    }
    QPushButton#paste:hover, QPushButton#cancel:hover, QPushButton#copyHostInvitation:hover, QPushButton#hostDeny:hover { background: #edf4ff; border-color: #a4bce4; }
    QPushButton#paste:pressed, QPushButton#cancel:pressed, QPushButton#copyHostInvitation:pressed, QPushButton#hostDeny:pressed { background: #e0ecff; }
    QPushButton#paste:focus, QPushButton#cancel:focus, QPushButton#copyHostInvitation:focus, QPushButton#hostDeny:focus { border-color: #3475e4; }
    QPushButton#paste:disabled { color: #667899; border-color: #dfe6f1; background: #f4f7fc; }
    QPushButton#join, QPushButton#hostCreate, QPushButton#hostApprove {
        background: #2463da; color: #ffffff; border: 2px solid transparent;
        border-radius: 8px; padding: 0 17px;
    }
    QPushButton#join:hover, QPushButton#hostCreate:hover, QPushButton#hostApprove:hover { background: #1956c5; }
    QPushButton#join:pressed, QPushButton#hostCreate:pressed, QPushButton#hostApprove:pressed { background: #1348ac; }
    QPushButton#join:focus, QPushButton#hostCreate:focus, QPushButton#hostApprove:focus { border-color: #8db5ff; }
    QPushButton#join:disabled { color: #43608f; background: #e2eafb; }
    QPushButton#hostEnd {
        background: #922f31; color: white; border: 2px solid transparent;
        border-radius: 8px; padding: 0 11px;
    }
    QPushButton#hostEnd:hover { background: #7c2225; }
    QPushButton#hostEnd:pressed { background: #61191d; }
    QPushButton#hostEnd:focus { border-color: #f3aab0; }
)";

QPushButton* roleButton(const QString& text, const char* name, Symbol symbol) {
    auto* button = new QPushButton(text);
    button->setObjectName(QString::fromLatin1(name));
    button->setProperty("roleNavigation", true);
    button->setFont(uiFont(14, QFont::DemiBold));
    button->setIcon(symbolIcon(symbol, QColor("#275cae"), 19));
    button->setIconSize(QSize(19, 19));
    button->setMinimumHeight(44);
    button->setCursor(Qt::PointingHandCursor);
    button->setCheckable(true);
    return button;
}

QPushButton* hostButton(const QString& text, const char* name) {
    auto* button = new QPushButton(text);
    button->setObjectName(QString::fromLatin1(name));
    button->setFont(uiFont(14, QFont::DemiBold));
    button->setMinimumHeight(41);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QVBoxLayout* hostCard(QFrame* card) {
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(17, 15, 17, 16);
    layout->setSpacing(10);
    return layout;
}

} // namespace

HomeWindow::HomeWindow(QWidget* parent)
    : QMainWindow(parent)
    , statusMessage_(idleMessage)
{
    setWindowTitle(QStringLiteral("EVP 远程"));
    setWindowIcon(symbolIcon(Symbol::Brand, QColor("#2865d5"), 24));
    setMinimumSize(724, 470);
    resize(960, 674);

    auto* home = new QWidget;
    home->setObjectName(QStringLiteral("home"));
    home->setFont(uiFont(14));
    home->setStyleSheet(QString::fromLatin1(homeStyle));
    setCentralWidget(home);
    auto* shell = new QHBoxLayout(home);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    auto* rail = new QWidget;
    rail->setObjectName(QStringLiteral("rail"));
    rail->setFixedWidth(184);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(16, 25, 16, 24);
    railLayout->setSpacing(0);
    shell->addWidget(rail);

    auto* brand = new QHBoxLayout;
    brand->setContentsMargins(4, 0, 0, 0);
    brand->setSpacing(10);
    auto* brandMark = label({}, "brandMark", 14);
    brandMark->setFixedSize(36, 36);
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setPixmap(symbolPixmap(Symbol::Brand, Qt::white, 23));
    brand->addWidget(brandMark);
    brand->addWidget(label(QStringLiteral("EVP 远程"), "brandName", 17, QFont::DemiBold), 1);
    railLayout->addLayout(brand);
    railLayout->addSpacing(34);
    deviceNavigation_ = roleButton(QStringLiteral("我的设备"), "deviceNavigation", Symbol::Display);
    viewerNavigation_ = roleButton(QStringLiteral("远程协助"), "navigation", Symbol::Connect);
    hostNavigation_ = roleButton(QStringLiteral("共享本机"), "hostNavigation", Symbol::Display);
    railLayout->addWidget(deviceNavigation_);
    railLayout->addSpacing(6);
    railLayout->addWidget(viewerNavigation_);
    railLayout->addSpacing(6);
    railLayout->addWidget(hostNavigation_);
    railLayout->addStretch();
    hostEndButton_ = hostButton(QStringLiteral("结束共享"), "hostEnd");
    hostEndButton_->setIcon(symbolIcon(Symbol::End, Qt::white, 17));
    hostEndButton_->setIconSize(QSize(17, 17));
    railLayout->addWidget(hostEndButton_);
    trustedShareStop_.reset(hostButton(QStringLiteral("正在共享本机 · 停止共享"), "trustedShareStop"));
    trustedShareStop_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                                      Qt::WindowDoesNotAcceptFocus);
    trustedShareStop_->setAttribute(Qt::WA_ShowWithoutActivating);
    trustedShareStop_->setAttribute(Qt::WA_QuitOnClose, false);
    trustedShareStop_->setStyleSheet(QStringLiteral(
        "QPushButton { color: white; background: #922f31; border: 0; padding: 0 14px; }"
        "QPushButton:hover { background: #7c2225; } QPushButton:pressed { background: #61191d; }"));
    trustedShareStop_->setIcon(symbolIcon(Symbol::End, Qt::white, 17));
    connect(trustedShareStop_.get(), &QPushButton::clicked, this, &HomeWindow::hostEndRequested);
    railLayout->addSpacing(16);
    auto* role = label(QStringLiteral("Windows"), "subtle", 12);
    role->setContentsMargins(8, 0, 0, 0);
    railLayout->addWidget(role);

    scroll_ = new QScrollArea;
    scroll_->setObjectName(QStringLiteral("homeScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    shell->addWidget(scroll_, 1);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("homeBody"));
    scroll_->setWidget(body);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(35, 32, 35, 29);
    bodyLayout->setSpacing(0);
    auto* content = new QWidget;
    content->setMaximumWidth(850);
    bodyLayout->addWidget(content);
    bodyLayout->addStretch();
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    contentLayout->addWidget(label(QStringLiteral("设备与连接"), "title", 29, QFont::DemiBold));
    contentLayout->addSpacing(18);
    banner_ = new QFrame;
    banner_->setObjectName(QStringLiteral("statusBanner"));
    auto* bannerLayout = new QHBoxLayout(banner_);
    bannerLayout->setContentsMargins(17, 14, 17, 14);
    bannerLayout->setSpacing(13);
    bannerIcon_ = label({}, "bannerGlyph", 15);
    bannerIcon_->setFixedSize(40, 40);
    bannerIcon_->setAlignment(Qt::AlignCenter);
    bannerLayout->addWidget(bannerIcon_, 0, Qt::AlignVCenter);
    auto* bannerText = new QVBoxLayout;
    bannerText->setSpacing(3);
    bannerTitle_ = label({}, "bannerTitle", 16, QFont::DemiBold);
    bannerMessage_ = label({}, "bannerMessage", 13);
    bannerMessage_->setTextFormat(Qt::PlainText);
    bannerMessage_->setWordWrap(true);
    bannerText->addWidget(bannerTitle_);
    bannerText->addWidget(bannerMessage_);
    bannerLayout->addLayout(bannerText, 1);
    contentLayout->addWidget(banner_);
    contentLayout->addSpacing(25);

    contentLayout->addWidget(label(QStringLiteral("本机设备"), "sectionTitle", 17, QFont::DemiBold));
    contentLayout->addSpacing(9);
    auto* deviceCard = new QFrame;
    deviceCard->setObjectName(QStringLiteral("card"));
    auto* deviceLayout = new QHBoxLayout(deviceCard);
    deviceLayout->setContentsMargins(16, 13, 16, 13);
    deviceLayout->setSpacing(13);
    auto* deviceIcon = label({}, "deviceIcon", 15);
    deviceIcon->setFixedSize(42, 42);
    deviceIcon->setAlignment(Qt::AlignCenter);
    deviceIcon->setPixmap(symbolPixmap(Symbol::Display, QColor("#2c63be"), 24));
    deviceLayout->addWidget(deviceIcon);
    auto* machine = new QVBoxLayout;
    machine->setSpacing(3);
    const auto hostName = QSysInfo::machineHostName();
    auto* machineName = label(hostName.isEmpty() ? QStringLiteral("设备名称未知") : hostName,
                              "machineName", 15, QFont::DemiBold);
    machineName->setWordWrap(true);
    machineName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    machine->addWidget(machineName);
    machine->addWidget(label(QStringLiteral("Windows"), "machinePlatform", 12));
    deviceLayout->addLayout(machine, 1);
    contentLayout->addWidget(deviceCard);
    contentLayout->addSpacing(25);

    pages_ = new QStackedWidget;
    contentLayout->addWidget(pages_);
    auto* viewerPage = new QWidget;
    auto* viewerLayout = new QVBoxLayout(viewerPage);
    viewerLayout->setContentsMargins(0, 0, 0, 0);
    viewerLayout->setSpacing(9);
    viewerLayout->addWidget(label(QStringLiteral("连接远程设备"), "sectionTitle", 17, QFont::DemiBold));
    auto* form = new QFrame;
    form->setObjectName(QStringLiteral("card"));
    auto* formLayout = new QVBoxLayout(form);
    formLayout->setContentsMargins(17, 15, 17, 16);
    formLayout->setSpacing(0);
    auto* invitationLabel = label(QStringLiteral("邀请码"), "fieldLabel", 13, QFont::DemiBold);
    invitation_ = new QLineEdit;
    invitation_->setObjectName(QStringLiteral("invitation"));
    invitation_->setAccessibleName(invitationLabel->text());
    invitation_->setPlaceholderText(QStringLiteral("输入或粘贴邀请码"));
    invitation_->setMaxLength(std::numeric_limits<int>::max());
    invitation_->setMinimumHeight(41);
    invitationLabel->setBuddy(invitation_);
    formLayout->addWidget(invitationLabel);
    formLayout->addSpacing(6);
    auto* invitationRow = new QHBoxLayout;
    invitationRow->setSpacing(8);
    invitationRow->addWidget(invitation_, 1);
    pasteButton_ = new QPushButton(QStringLiteral("粘贴"));
    pasteButton_->setObjectName(QStringLiteral("paste"));
    pasteButton_->setAccessibleName(QStringLiteral("粘贴邀请码"));
    pasteButton_->setFont(uiFont(13, QFont::DemiBold));
    pasteButton_->setMinimumHeight(41);
    pasteButton_->setIcon(symbolIcon(Symbol::Clipboard, QColor("#456594"), 17));
    pasteButton_->setIconSize(QSize(17, 17));
    pasteButton_->setCursor(Qt::PointingHandCursor);
    invitationRow->addWidget(pasteButton_);
    formLayout->addLayout(invitationRow);
    formLayout->addSpacing(13);

    auto* serverLabel = label(QStringLiteral("信令服务器"), "fieldLabel", 13, QFont::DemiBold);
    signalingUrl_ = new QLineEdit;
    signalingUrl_->setObjectName(QStringLiteral("signalingUrl"));
    signalingUrl_->setAccessibleName(serverLabel->text());
    signalingUrl_->setPlaceholderText(QStringLiteral("wss://服务器地址/"));
    signalingUrl_->setMinimumHeight(41);
    serverLabel->setBuddy(signalingUrl_);
    formLayout->addWidget(serverLabel);
    formLayout->addSpacing(6);
    formLayout->addWidget(signalingUrl_);
    formLayout->addSpacing(15);
    joinButton_ = new QPushButton(QStringLiteral("请求连接"));
    joinButton_->setObjectName(QStringLiteral("join"));
    joinButton_->setFont(uiFont(14, QFont::DemiBold));
    joinButton_->setMinimumSize(119, 41);
    joinButton_->setIcon(symbolIcon(Symbol::Arrow, Qt::white, 18));
    joinButton_->setIconSize(QSize(18, 18));
    joinButton_->setLayoutDirection(Qt::RightToLeft);
    joinButton_->setCursor(Qt::PointingHandCursor);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addWidget(joinButton_);
    cancelButton_ = new QPushButton(QStringLiteral("取消连接"));
    cancelButton_->setObjectName(QStringLiteral("cancel"));
    cancelButton_->setFont(uiFont(13, QFont::DemiBold));
    cancelButton_->setMinimumHeight(41);
    cancelButton_->setCursor(Qt::PointingHandCursor);
    actions->addWidget(cancelButton_);
    actions->addStretch();
    formLayout->addLayout(actions);
    viewerLayout->addWidget(form);
    viewerLayout->addStretch();
    pages_->addWidget(viewerPage);
    pages_->addWidget(createHostPage());
    devicePage_ = new DevicePage;
    pages_->addWidget(devicePage_);

    setTabOrder(deviceNavigation_, viewerNavigation_);
    setTabOrder(viewerNavigation_, hostNavigation_);
    setTabOrder(hostNavigation_, invitation_);
    setTabOrder(invitation_, pasteButton_);
    setTabOrder(pasteButton_, signalingUrl_);
    setTabOrder(signalingUrl_, joinButton_);
    setTabOrder(joinButton_, cancelButton_);
    connect(viewerNavigation_, &QPushButton::clicked, this, [this] { switchRole(false); });
    connect(hostNavigation_, &QPushButton::clicked, this, [this] { switchRole(true); });
    connect(deviceNavigation_, &QPushButton::clicked, this, &HomeWindow::showDevicePage);
    connect(devicePage_, &DevicePage::returnRequested, this, &HomeWindow::sessionActivationRequested);
    connect(hostEndButton_, &QPushButton::clicked, this, &HomeWindow::hostEndRequested);
    connect(signalingUrl_, &QLineEdit::textChanged, hostSignalingUrl_, &QLineEdit::setText);
    connect(hostSignalingUrl_, &QLineEdit::textChanged, signalingUrl_, &QLineEdit::setText);
    connect(pasteButton_, &QPushButton::clicked, this, &HomeWindow::pasteInvitation);
    connect(joinButton_, &QPushButton::clicked, this, [this] {
        if (sessionActive_) {
            emit sessionActivationRequested();
        } else {
            requestJoin();
        }
    });
    connect(cancelButton_, &QPushButton::clicked, this, &HomeWindow::cancelRequested);
    connect(invitation_, &QLineEdit::returnPressed, this, &HomeWindow::requestJoin);
    connect(signalingUrl_, &QLineEdit::returnPressed, this, &HomeWindow::requestJoin);
    invitation_->setFocus(Qt::OtherFocusReason);
    updateForm();
    updateBanner();
}

HomeWindow::~HomeWindow()
{
    if (notificationWindow_)
        WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(notificationWindow_));
}

bool HomeWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    const auto* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_WTSSESSION_CHANGE) {
        switch (msg->wParam) {
        case WTS_SESSION_LOCK:
        case WTS_SESSION_LOGOFF:
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT:
            emit hostDesktopUnavailable();
            *result = 0;
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

QWidget* HomeWindow::createHostPage()
{
    auto* page = new QWidget;
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(9);
    pageLayout->addWidget(label(QStringLiteral("共享本机"), "sectionTitle", 17, QFont::DemiBold));

    auto* setup = new QFrame;
    hostSetup_ = setup;
    auto* setupLayout = hostCard(setup);
    auto* serverLabel = label(QStringLiteral("信令服务器"), "fieldLabel", 13, QFont::DemiBold);
    hostSignalingUrl_ = new QLineEdit;
    hostSignalingUrl_->setObjectName(QStringLiteral("hostSignalingUrl"));
    hostSignalingUrl_->setAccessibleName(serverLabel->text());
    hostSignalingUrl_->setPlaceholderText(QStringLiteral("wss://服务器地址/"));
    hostSignalingUrl_->setMinimumHeight(41);
    serverLabel->setBuddy(hostSignalingUrl_);
    setupLayout->addWidget(serverLabel);
    setupLayout->addWidget(hostSignalingUrl_);

    auto* keyLabel = label(QStringLiteral("部署密钥"), "fieldLabel", 13, QFont::DemiBold);
    hostKey_ = new QLineEdit;
    hostKey_->setObjectName(QStringLiteral("hostKey"));
    hostKey_->setAccessibleName(keyLabel->text());
    hostKey_->setPlaceholderText(QStringLiteral("输入部署时配置的主机密钥"));
    hostKey_->setEchoMode(QLineEdit::Password);
    hostKey_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText |
                                  Qt::ImhNoAutoUppercase);
    hostKey_->setMinimumHeight(41);
    keyLabel->setBuddy(hostKey_);
    setupLayout->addWidget(keyLabel);
    setupLayout->addWidget(hostKey_);
    auto* create = hostButton(QStringLiteral("创建邀请码"), "hostCreate");
    setupLayout->addWidget(create, 0, Qt::AlignLeft);
    pageLayout->addWidget(setup);

    auto* invite = new QFrame;
    hostInvite_ = invite;
    auto* inviteLayout = hostCard(invite);
    auto* inviteLabel = label(QStringLiteral("一次性邀请码"), "fieldLabel", 13, QFont::DemiBold);
    hostInvitation_ = new QLineEdit;
    hostInvitation_->setObjectName(QStringLiteral("hostInvitation"));
    hostInvitation_->setAccessibleName(inviteLabel->text());
    hostInvitation_->setReadOnly(true);
    hostInvitation_->setMinimumHeight(41);
    inviteLabel->setBuddy(hostInvitation_);
    inviteLayout->addWidget(inviteLabel);
    auto* inviteRow = new QHBoxLayout;
    inviteRow->setSpacing(8);
    inviteRow->addWidget(hostInvitation_, 1);
    auto* copy = hostButton(QStringLiteral("复制邀请码"), "copyHostInvitation");
    copy->setIcon(symbolIcon(Symbol::Clipboard, QColor("#456594"), 17));
    copy->setIconSize(QSize(17, 17));
    inviteRow->addWidget(copy);
    inviteLayout->addLayout(inviteRow);
    auto* inviteHint = label(QStringLiteral("请仅将邀请码发给你信任的人。"), "subtle", 13);
    inviteHint->setWordWrap(true);
    inviteLayout->addWidget(inviteHint);
    pageLayout->addWidget(invite);

    auto* consent = new QFrame;
    hostConsent_ = consent;
    auto* consentLayout = hostCard(consent);
    consentLayout->addWidget(
        label(QStringLiteral("收到远程控制请求"), "fieldLabel", 16, QFont::DemiBold));
    auto* disclosure = label(
        QStringLiteral("使用此邀请码的设备请求连接。同意后，对方可以查看整个屏幕，并使用键盘和鼠标控制本机。"),
        "subtle", 14);
    disclosure->setWordWrap(true);
    consentLayout->addWidget(disclosure);
    shareAudio_ = new QCheckBox(QStringLiteral("共享系统声音"));
    shareAudio_->setObjectName(QStringLiteral("hostShareAudio"));
    shareAudio_->setChecked(true);
    consentLayout->addWidget(shareAudio_);
    highRefresh_ = new QCheckBox(QStringLiteral("原生分辨率，目标 144 帧/秒"));
    highRefresh_->setObjectName(QStringLiteral("hostHighRefresh"));
    relayOnly_ = new QCheckBox(QStringLiteral("仅使用 TURN 中继"));
    relayOnly_->setObjectName(QStringLiteral("hostRelayOnly"));
    consentLayout->addWidget(highRefresh_);
    consentLayout->addWidget(relayOnly_);
    auto* decisions = new QHBoxLayout;
    decisions->setSpacing(8);
    auto* approve = hostButton(QStringLiteral("同意并开始共享"), "hostApprove");
    auto* deny = hostButton(QStringLiteral("拒绝"), "hostDeny");
    decisions->addWidget(approve);
    decisions->addWidget(deny);
    decisions->addStretch();
    consentLayout->addLayout(decisions);
    pageLayout->addWidget(consent);
    pageLayout->addStretch();

    setTabOrder(hostSignalingUrl_, hostKey_);
    setTabOrder(hostKey_, create);
    setTabOrder(hostInvitation_, copy);
    setTabOrder(highRefresh_, relayOnly_);
    setTabOrder(relayOnly_, approve);
    setTabOrder(approve, deny);
    connect(create, &QPushButton::clicked, this, &HomeWindow::requestHost);
    connect(hostKey_, &QLineEdit::returnPressed, this, &HomeWindow::requestHost);
    connect(hostSignalingUrl_, &QLineEdit::returnPressed, hostKey_,
            [this] { hostKey_->setFocus(); });
    connect(copy, &QPushButton::clicked, this, &HomeWindow::copyHostInvitation);
    connect(approve, &QPushButton::clicked, this, [this] { respondToHostRequest(true); });
    connect(deny, &QPushButton::clicked, this, [this] { respondToHostRequest(false); });
    return page;
}

void HomeWindow::setSignalingUrl(const QUrl& url)
{
    signalingUrl_->setText(url.toString(QUrl::FullyEncoded));
    devicePage_->setServer(url);
}

DevicePage& HomeWindow::devices()
{
    return *devicePage_;
}

void HomeWindow::showDevicePage()
{
    devicePageSelected_ = true;
    updateForm();
    updateBanner();
    scroll_->ensureVisible(0, 0, 0, 0);
}

void HomeWindow::setBusy(bool busy, bool trusted)
{
    if (busy_ == busy || (busy && hostBusy_)) {
        return;
    }
    if (busy) {
        if (trusted)
            hostPage_ = false;
        else
            switchRole(false);
    }
    busy_ = busy;
    trustedSession_ = busy && trusted;
    if (busy) {
        failed_ = false;
        statusMessage_ = trusted ? QStringLiteral("正在建立可信设备连接。") : waitingMessage;
    } else {
        sessionActive_ = false;
        if (!failed_) {
            statusMessage_ = idleMessage;
        }
    }
    if (trustedSession_)
        showDevicePage();
    else {
        updateForm();
        updateBanner();
    }
}

void HomeWindow::setSessionActive(bool active)
{
    if (sessionActive_ == active) {
        return;
    }
    if (active) {
        setBusy(true);
        failed_ = false;
        statusMessage_ = QStringLiteral("可返回独立控制窗口继续会话。");
    } else if (!failed_) {
        statusMessage_ = waitingMessage;
    }
    sessionActive_ = active;
    updateForm();
    updateBanner();
}

bool HomeWindow::setHostBusy(bool busy, bool trusted)
{
    if (hostBusy_ == busy || (busy && busy_)) {
        return hostBusy_ == busy;
    }
    hostKey_->setText({});
    if (busy) {
        const WId id = winId();
        if (!WTSRegisterSessionNotification(reinterpret_cast<HWND>(id), NOTIFY_FOR_THIS_SESSION))
            return false;
        notificationWindow_ = id;
        if (trusted)
            hostPage_ = true;
        else
            switchRole(true);
        failed_ = false;
        statusMessage_ = trusted ? QStringLiteral("已验证可信设备，正在准备共享。")
                                 : QStringLiteral("正在创建邀请码。");
    } else {
        if (notificationWindow_)
            WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(std::exchange(notificationWindow_, 0)));
        hostActive_ = false;
        hostRequestId_ = 0;
        hostInvitation_->clear();
        shareAudio_->setChecked(true);
        highRefresh_->setChecked(false);
        relayOnly_->setChecked(false);
        if (!failed_) {
            statusMessage_ = hostIdleMessage;
        }
    }
    hostBusy_ = busy;
    trustedSession_ = busy && trusted;
    if (trustedSession_)
        showDevicePage();
    else {
        updateForm();
        updateBanner();
    }
    return true;
}

void HomeWindow::setHostInvitation(const QString& invitation)
{
    if (!hostBusy_ || hostRequestId_ || hostActive_) {
        return;
    }
    hostInvitation_->setText(invitation);
    updateForm();
    if (!invitation.isEmpty()) {
        scroll_->ensureWidgetVisible(hostInvitation_);
    }
}

void HomeWindow::setHostPending(quint64 requestId)
{
    if (!hostBusy_ || hostActive_) {
        return;
    }
    hostRequestId_ = requestId;
    if (requestId) {
        hostInvitation_->clear();
    }
    updateForm();
    updateBanner();
    if (requestId) {
        scroll_->ensureWidgetVisible(hostConsent_, 0, 12);
        QApplication::alert(this);
    }
}

void HomeWindow::setHostActive(bool active)
{
    if (!hostBusy_ || hostActive_ == active) {
        return;
    }
    hostActive_ = active;
    if (active) {
        hostInvitation_->clear();
        hostRequestId_ = 0;
    }
    updateForm();
    updateBanner();
}

void HomeWindow::setStatus(const QString& message)
{
    failed_ = false;
    statusMessage_ = message;
    updateBanner();
}

void HomeWindow::showFailure(const QString& message)
{
    failed_ = true;
    statusMessage_ = message;
    updateBanner();
    scroll_->ensureWidgetVisible(banner_, 0, 12);
}

void HomeWindow::switchRole(bool host)
{
    if (trustedSession_ || (host && busy_) || (!host && hostBusy_)) {
        updateForm();
        return;
    }
    if (host != hostPage_ || (devicePageSelected_ && !hostBusy_ && !busy_)) {
        hostPage_ = host;
        hostKey_->setText({});
        failed_ = false;
        statusMessage_ = host ? hostIdleMessage : idleMessage;
    }
    devicePageSelected_ = false;
    updateBanner();
    updateForm();
    if (hostBusy_) {
        if (hostRequestId_) {
            scroll_->ensureWidgetVisible(hostConsent_, 0, 12);
        } else {
            hostEndButton_->setFocus(Qt::ShortcutFocusReason);
        }
        return;
    }
    QWidget* target = host ? hostKey_ : invitation_;
    if (busy_) {
        target = sessionActive_ ? joinButton_ : cancelButton_;
    }
    target->setFocus(Qt::ShortcutFocusReason);
    scroll_->ensureWidgetVisible(target);
}

QUrl HomeWindow::requestedSignalingUrl(QLineEdit* field)
{
    const QUrl url(field->text().trimmed(), QUrl::StrictMode);
    const auto host = url.host();
    const bool loopback = host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1")
                         || host == QStringLiteral("::1");
    const bool allowedScheme = url.scheme() == QStringLiteral("wss")
                               || (loopback && url.scheme() == QStringLiteral("ws"));
    if (!url.isValid() || host.isEmpty() || !allowedScheme || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/")) {
        showFailure(QStringLiteral("请输入部署使用的有效 wss:// 信令地址。"));
        field->setFocus();
        return {};
    }
    return url;
}

void HomeWindow::requestJoin()
{
    if (busy_ || hostBusy_ || hostPage_) {
        return;
    }

    const auto invitation = invitation_->text().trimmed();
    if (invitation.isEmpty()) {
        showFailure(QStringLiteral("请输入对方提供的邀请码。"));
        invitation_->setFocus();
        return;
    }

    const auto url = requestedSignalingUrl(signalingUrl_);
    if (!url.isEmpty()) {
        emit joinRequested(url, invitation);
    }
}

void HomeWindow::requestHost()
{
    if (busy_ || hostBusy_ || !hostPage_) {
        return;
    }
    const auto key = hostKey_->text();
    if (key.trimmed().isEmpty()) {
        showFailure(QStringLiteral("请输入此信令服务的部署密钥。"));
        hostKey_->setFocus();
        return;
    }
    const auto url = requestedSignalingUrl(hostSignalingUrl_);
    if (!url.isEmpty()) {
        hostKey_->setText({});
        emit hostRequested(url, key);
    }
}

void HomeWindow::pasteInvitation()
{
    const auto text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) {
        showFailure(QStringLiteral("剪贴板中没有邀请码。"));
    } else {
        invitation_->setText(text);
        setStatus(idleMessage);
    }
    invitation_->setFocus();
}

void HomeWindow::copyHostInvitation()
{
    const auto invitation = hostInvitation_->text();
    if (!invitation.isEmpty() && hostBusy_ && !hostRequestId_) {
        QApplication::clipboard()->setText(invitation);
        setStatus(QStringLiteral("已复制邀请码，等待对方发起连接。"));
    }
}

void HomeWindow::respondToHostRequest(bool approved)
{
    if (!hostBusy_ || !hostRequestId_ || hostActive_) {
        return;
    }
    const auto requestId = std::exchange(hostRequestId_, 0);
    updateForm();
    updateBanner();
    if (approved) {
        emit hostApproved(requestId, highRefresh_->isChecked(), relayOnly_->isChecked(), shareAudio_->isChecked());
    } else {
        emit hostDenied(requestId);
    }
}

void HomeWindow::updateForm()
{
    const int selected = devicePageSelected_ ? 2 : hostPage_ ? 1 : 0;
    for (int index = 0; index < pages_->count(); ++index)
        pages_->widget(index)->setSizePolicy(QSizePolicy::Preferred,
                                            index == selected ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    pages_->setCurrentIndex(selected);
    deviceNavigation_->setChecked(devicePageSelected_);
    viewerNavigation_->setChecked(!devicePageSelected_ && !hostPage_);
    hostNavigation_->setChecked(!devicePageSelected_ && hostPage_);
    viewerNavigation_->setEnabled(!hostBusy_ && !trustedSession_);
    hostNavigation_->setEnabled(!busy_ && !trustedSession_);
    devicePage_->setReturnVisible(sessionActive_);
    hostSetup_->setVisible(!hostBusy_);
    hostInvite_->setVisible(hostBusy_ && !hostInvitation_->text().isEmpty());
    hostConsent_->setVisible(hostBusy_ && hostRequestId_);
    hostEndButton_->setVisible(hostBusy_);
    if (hostActive_ && trustedSession_) {
        if (!trustedShareStop_->isVisible()) {
            trustedShareStop_->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
            trustedShareStop_->adjustSize();
            const auto area = screen()->availableGeometry();
            trustedShareStop_->move(area.right() + 1 - trustedShareStop_->width() - 12, area.top() + 12);
            trustedShareStop_->show();
        }
    } else {
        trustedShareStop_->hide();
    }
    hostEndButton_->setText(hostActive_ ? QStringLiteral("结束共享")
                                      : QStringLiteral("取消共享"));
    invitation_->setEnabled(!busy_);
    signalingUrl_->setEnabled(!busy_);
    pasteButton_->setEnabled(!busy_);
    joinButton_->setEnabled(!busy_ || sessionActive_);
    cancelButton_->setVisible(busy_ && !sessionActive_);
    if (sessionActive_) {
        joinButton_->setText(QStringLiteral("返回控制窗口"));
    } else {
        joinButton_->setText(busy_ ? QStringLiteral("正在连接") : QStringLiteral("请求连接"));
    }
}

void HomeWindow::updateBanner()
{
    if (failed_) {
        banner_->setStyleSheet(QStringLiteral(R"(
            QFrame#statusBanner { background: #fff1ed; border: 1px solid #f0d8cf; border-radius: 13px; }
            QLabel#bannerGlyph { background: #fbe0d7; border-radius: 10px; }
            QLabel#bannerTitle { color: #82372b; }
            QLabel#bannerMessage { color: #75473e; }
        )"));
        bannerIcon_->setPixmap(symbolPixmap(Symbol::Warning, QColor("#a54737"), 22));
        bannerTitle_->setText(hostPage_ ? QStringLiteral("共享操作失败") : QStringLiteral("无法连接"));
    } else {
        banner_->setStyleSheet(QStringLiteral(R"(
            QFrame#statusBanner { background: #eaf2ff; border: 1px solid #dce9fd; border-radius: 13px; }
            QLabel#bannerGlyph { background: #d9e8ff; border-radius: 10px; }
            QLabel#bannerTitle { color: #1e427c; }
            QLabel#bannerMessage { color: #405b85; }
        )"));
        bannerIcon_->setPixmap(symbolPixmap(Symbol::Connect, QColor("#2864c2"), 24));
        if (hostPage_) {
            if (hostActive_) {
                bannerTitle_->setText(QStringLiteral("正在共享本机"));
            } else if (hostRequestId_) {
                bannerTitle_->setText(QStringLiteral("等待确认"));
            } else {
                bannerTitle_->setText(hostBusy_ ? QStringLiteral("共享准备中") : QStringLiteral("等待共享"));
            }
        } else if (sessionActive_) {
            bannerTitle_->setText(QStringLiteral("控制会话进行中"));
        } else {
            bannerTitle_->setText(busy_ ? QStringLiteral("正在连接") : QStringLiteral("等待连接"));
        }
    }
    banner_->setVisible(!devicePageSelected_ || hostBusy_ || busy_ || failed_);
    bannerMessage_->setText(statusMessage_);
}
