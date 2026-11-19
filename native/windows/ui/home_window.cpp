#include "home_window.h"

#include "visual_style.h"

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSysInfo>
#include <QVBoxLayout>

#include <limits>

namespace {

using native_ui::label;
using native_ui::Symbol;
using native_ui::symbolIcon;
using native_ui::symbolPixmap;
using native_ui::uiFont;

const auto idleMessage = QStringLiteral("输入对方发来的邀请码，在对方确认后开始控制。");
const auto waitingMessage = QStringLiteral("正在向对方发送连接请求。");

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
    QPushButton#navigation {
        background: #dbe8ff; color: #214c98; border: 1px solid transparent;
        border-radius: 9px; padding: 0 12px; text-align: left;
    }
    QPushButton#navigation:hover { background: #cddfff; }
    QPushButton#navigation:pressed { background: #c2d6fa; }
    QPushButton#navigation:focus { border-color: #3475e4; }
    QPushButton#navigation:disabled { color: #4a6592; }
    QPushButton#paste, QPushButton#cancel {
        background: #f8faff; color: #284675; border: 1px solid #ccd9ec;
        border-radius: 8px; padding: 0 12px;
    }
    QPushButton#paste:hover, QPushButton#cancel:hover { background: #edf4ff; border-color: #a4bce4; }
    QPushButton#paste:pressed, QPushButton#cancel:pressed { background: #e0ecff; }
    QPushButton#paste:focus, QPushButton#cancel:focus { border-color: #3475e4; }
    QPushButton#paste:disabled { color: #667899; border-color: #dfe6f1; background: #f4f7fc; }
    QPushButton#join {
        background: #2463da; color: #ffffff; border: 2px solid transparent;
        border-radius: 8px; padding: 0 17px;
    }
    QPushButton#join:hover { background: #1956c5; }
    QPushButton#join:pressed { background: #1348ac; }
    QPushButton#join:focus { border-color: #8db5ff; }
    QPushButton#join:disabled { color: #43608f; background: #e2eafb; }
)";

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
    auto* navigation = new QPushButton(QStringLiteral("远程连接"));
    navigation->setObjectName(QStringLiteral("navigation"));
    navigation->setFont(uiFont(14, QFont::DemiBold));
    navigation->setIcon(symbolIcon(Symbol::Connect, QColor("#275cae"), 19));
    navigation->setIconSize(QSize(19, 19));
    navigation->setMinimumHeight(44);
    navigation->setCursor(Qt::PointingHandCursor);
    railLayout->addWidget(navigation);
    railLayout->addStretch();
    auto* role = label(QStringLiteral("Windows 控制端"), "subtle", 12);
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

    contentLayout->addWidget(label(QStringLiteral("连接远程设备"), "sectionTitle", 17, QFont::DemiBold));
    contentLayout->addSpacing(9);
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
    contentLayout->addWidget(form);

    setTabOrder(navigation, invitation_);
    setTabOrder(invitation_, pasteButton_);
    setTabOrder(pasteButton_, signalingUrl_);
    setTabOrder(signalingUrl_, joinButton_);
    setTabOrder(joinButton_, cancelButton_);
    connect(navigation, &QPushButton::clicked, this, [this] {
        QWidget* target = invitation_;
        if (busy_) {
            target = sessionActive_ ? joinButton_ : cancelButton_;
        }
        target->setFocus(Qt::ShortcutFocusReason);
        scroll_->ensureWidgetVisible(target);
    });
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

void HomeWindow::setSignalingUrl(const QUrl& url)
{
    signalingUrl_->setText(url.toString(QUrl::FullyEncoded));
}

void HomeWindow::setBusy(bool busy)
{
    if (busy_ == busy) {
        return;
    }
    busy_ = busy;
    if (busy) {
        failed_ = false;
        statusMessage_ = waitingMessage;
    } else {
        sessionActive_ = false;
        if (!failed_) {
            statusMessage_ = idleMessage;
        }
    }
    updateForm();
    updateBanner();
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

void HomeWindow::requestJoin()
{
    if (busy_) {
        return;
    }

    const auto invitation = invitation_->text().trimmed();
    if (invitation.isEmpty()) {
        showFailure(QStringLiteral("请输入对方提供的邀请码。"));
        invitation_->setFocus();
        return;
    }

    const QUrl url(signalingUrl_->text().trimmed(), QUrl::StrictMode);
    const auto host = url.host();
    const bool loopback = host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1")
                         || host == QStringLiteral("::1");
    const bool allowedScheme = url.scheme() == QStringLiteral("wss")
                               || (loopback && url.scheme() == QStringLiteral("ws"));
    if (!url.isValid() || host.isEmpty() || !allowedScheme || !url.userInfo().isEmpty()
        || url.hasFragment()) {
        showFailure(QStringLiteral("请输入对方使用的有效 wss:// 信令地址。"));
        signalingUrl_->setFocus();
        return;
    }

    emit joinRequested(url, invitation);
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

void HomeWindow::updateForm()
{
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
        bannerTitle_->setText(QStringLiteral("无法连接"));
    } else {
        banner_->setStyleSheet(QStringLiteral(R"(
            QFrame#statusBanner { background: #eaf2ff; border: 1px solid #dce9fd; border-radius: 13px; }
            QLabel#bannerGlyph { background: #d9e8ff; border-radius: 10px; }
            QLabel#bannerTitle { color: #1e427c; }
            QLabel#bannerMessage { color: #405b85; }
        )"));
        bannerIcon_->setPixmap(symbolPixmap(Symbol::Connect, QColor("#2864c2"), 24));
        if (sessionActive_) {
            bannerTitle_->setText(QStringLiteral("控制会话进行中"));
        } else {
            bannerTitle_->setText(busy_ ? QStringLiteral("正在连接") : QStringLiteral("等待连接"));
        }
    }
    bannerMessage_->setText(statusMessage_);
}
