#include "home_window.h"

#include "device_page.h"
#include "devices/device_identity.h"
#include "floating_surface.h"
#include "select_field.h"
#include "visual_style.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFormLayout>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyleOptionTab>
#include <QSysInfo>
#include <QTabBar>
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

const auto idleMessage = QStringLiteral("输入对方发来的一次性邀请码，请求控制对方的设备。");
const auto waitingMessage = QStringLiteral("正在向对方发送连接请求。");
const auto hostIdleMessage = QStringLiteral("设置有效期和确认方式，将一次性邀请码发给你信任的人。");

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
    QTabBar#assistanceTabs::tab {
        min-height: 34px; padding: 0 18px; margin-right: 4px;
        color: #405777; background: transparent;
        border: 1px solid transparent; border-bottom-width: 3px; border-radius: 5px;
    }
    QTabBar#assistanceTabs::tab:hover { background: #edf3fd; color: #214c98; }
    QTabBar#assistanceTabs::tab:selected {
        background: #e4eeff; color: #174ca8; border-bottom-color: #2865d5;
    }
    QTabBar#assistanceTabs::tab:disabled { background: transparent; color: #78869c; }
    QPushButton#paste, QPushButton#cancel, QPushButton#copyHostInvitation, QPushButton#hostDeny {
        background: #f8faff; color: #284675; border: 1px solid #ccd9ec;
        border-radius: 8px; padding: 0 12px;
    }
    QPushButton#paste:hover, QPushButton#cancel:hover, QPushButton#copyHostInvitation:hover, QPushButton#hostDeny:hover { background: #edf4ff; border-color: #a4bce4; }
    QPushButton#paste:pressed, QPushButton#cancel:pressed, QPushButton#copyHostInvitation:pressed, QPushButton#hostDeny:pressed { background: #e0ecff; }
    QPushButton#paste:focus, QPushButton#cancel:focus, QPushButton#copyHostInvitation:focus, QPushButton#hostDeny:focus { border-color: #3475e4; }
    QPushButton#paste:disabled { color: #667899; border-color: #dfe6f1; background: #f4f7fc; }
    QPushButton#join, QPushButton#hostCreate, QPushButton#hostApprove, QPushButton#saveSettings {
        background: #2463da; color: #ffffff; border: 2px solid transparent;
        border-radius: 8px; padding: 0 17px;
    }
    QPushButton#join:hover, QPushButton#hostCreate:hover, QPushButton#hostApprove:hover, QPushButton#saveSettings:hover { background: #1956c5; }
    QPushButton#join:pressed, QPushButton#hostCreate:pressed, QPushButton#hostApprove:pressed, QPushButton#saveSettings:pressed { background: #1348ac; }
    QPushButton#join:focus, QPushButton#hostCreate:focus, QPushButton#hostApprove:focus, QPushButton#saveSettings:focus { border-color: #8db5ff; }
    QPushButton#join:disabled, QPushButton#saveSettings:disabled { color: #43608f; background: #e2eafb; }
    QPushButton#hostEnd {
        background: #922f31; color: white; border: 2px solid transparent;
        border-radius: 8px; padding: 0 11px;
    }
    QPushButton#hostEnd:hover { background: #7c2225; }
    QPushButton#hostEnd:pressed { background: #61191d; }
    QPushButton#hostEnd:focus { border-color: #f3aab0; }
)";

class AssistanceTabs final : public QTabBar {
protected:
    void initStyleOption(QStyleOptionTab* option, int index) const override {
        QTabBar::initStyleOption(option, index);
        option->state.setFlag(QStyle::State_HasFocus, false);
    }
    void focusInEvent(QFocusEvent* event) override {
        if (event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason ||
            event->reason() == Qt::ShortcutFocusReason)
            keyboardFocus_ = true;
        else if (event->reason() == Qt::MouseFocusReason)
            keyboardFocus_ = false;
        QTabBar::focusInEvent(event);
        update();
    }
    void mousePressEvent(QMouseEvent* event) override {
        keyboardFocus_ = false;
        QTabBar::mousePressEvent(event);
        update();
    }
    void keyPressEvent(QKeyEvent* event) override {
        keyboardFocus_ = true;
        QTabBar::keyPressEvent(event);
        update();
    }
    void paintEvent(QPaintEvent* event) override {
        QTabBar::paintEvent(event);
        if (!hasFocus() || !keyboardFocus_ || currentIndex() < 0)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#2865d5"), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(tabRect(currentIndex())).adjusted(1, 1, -5, -2), 5, 5);
    }

private:
    bool keyboardFocus_ = false;
};

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
    assistanceNavigation_ = roleButton(QStringLiteral("远程协助"), "navigation", Symbol::Connect);
    railLayout->addWidget(deviceNavigation_);
    railLayout->addSpacing(6);
    railLayout->addWidget(assistanceNavigation_);
    railLayout->addStretch();
    hostEndButton_ = hostButton(QStringLiteral("结束共享"), "hostEnd");
    hostEndButton_->setIcon(symbolIcon(Symbol::End, Qt::white, 17));
    hostEndButton_->setIconSize(QSize(17, 17));
    railLayout->addWidget(hostEndButton_);
    shareIndicator_ = std::make_unique<QWidget>(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    shareIndicator_->setAttribute(Qt::WA_ShowWithoutActivating);
    shareIndicator_->setAttribute(Qt::WA_QuitOnClose, false);
    auto* indicatorLayout = new QVBoxLayout(shareIndicator_.get());
    indicatorLayout->setContentsMargins(0, 0, 0, 0);
    auto* stop = hostButton(QStringLiteral("正在共享本机 · 停止共享"), "trustedShareStop");
    indicatorLayout->addWidget(stop);
    stop->setStyleSheet(QStringLiteral(
        "QPushButton { color: white; background: #922f31; border: 0; border-radius: 8px; padding: 0 14px; }"
        "QPushButton:hover { background: #7c2225; } QPushButton:pressed { background: #61191d; }"));
    stop->setIcon(symbolIcon(Symbol::End, Qt::white, 17));
    native_ui::styleFloatingSurface(*shareIndicator_, *stop);
    connect(stop, &QPushButton::clicked, this, &HomeWindow::hostEndRequested);
    railLayout->addSpacing(16);
    auto* role = label(QStringLiteral("Windows"), "subtle", 12);
    role->setContentsMargins(8, 0, 0, 0);
    railLayout->addWidget(role);
    railLayout->addSpacing(6);
    settingsNavigation_ = roleButton(QStringLiteral("设置"), "settingsNavigation", Symbol::Settings);
    railLayout->addWidget(settingsNavigation_);

    auto* mainLayout = new QVBoxLayout;
    mainLayout->setSpacing(0);
    shell->addLayout(mainLayout, 1);
    assistanceHeader_ = new QWidget;
    auto* headerLayout = new QVBoxLayout(assistanceHeader_);
    headerLayout->setContentsMargins(35, 24, 35, 0);
    headerLayout->setSpacing(12);
    headerLayout->addWidget(label(QStringLiteral("远程协助"), "title", 29, QFont::DemiBold));
    assistanceTabs_ = new AssistanceTabs;
    assistanceTabs_->setObjectName(QStringLiteral("assistanceTabs"));
    assistanceTabs_->setAccessibleName(QStringLiteral("远程协助"));
    assistanceTabs_->setFont(uiFont(14, QFont::DemiBold));
    assistanceTabs_->setFocusPolicy(Qt::StrongFocus);
    assistanceTabs_->setExpanding(false);
    assistanceTabs_->setDrawBase(false);
    assistanceTabs_->addTab(QStringLiteral("远程协助"));
    assistanceTabs_->addTab(QStringLiteral("共享本机"));
    headerLayout->addWidget(assistanceTabs_);
    mainLayout->addWidget(assistanceHeader_);

    scroll_ = new QScrollArea;
    scroll_->setObjectName(QStringLiteral("homeScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mainLayout->addWidget(scroll_, 1);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("homeBody"));
    scroll_->setWidget(body);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(35, 16, 35, 29);
    bodyLayout->setSpacing(0);
    auto* content = new QWidget;
    content->setMaximumWidth(850);
    bodyLayout->addWidget(content, 0, Qt::AlignTop);
    bodyLayout->addStretch();
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(13);
    contentLayout->setAlignment(Qt::AlignTop);

    deviceTitle_ = label(QStringLiteral("设备与连接"), "title", 29, QFont::DemiBold);
    deviceTitle_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    deviceTitle_->setContentsMargins(0, 8, 0, 0);
    contentLayout->addWidget(deviceTitle_);
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
    deviceSummary_ = new QWidget;
    deviceSummary_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* summaryLayout = new QVBoxLayout(deviceSummary_);
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    summaryLayout->setSpacing(9);
    summaryLayout->addWidget(label(QStringLiteral("本机设备"), "sectionTitle", 17, QFont::DemiBold));
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
    summaryLayout->addWidget(deviceCard);
    contentLayout->addWidget(deviceSummary_);

    viewerFormPage_ = new QWidget;
    auto* viewerLayout = new QVBoxLayout(viewerFormPage_);
    viewerLayout->setContentsMargins(0, 0, 0, 0);
    viewerLayout->setSpacing(9);
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
    contentLayout->addWidget(viewerFormPage_);
    hostFormPage_ = createHostPage();
    contentLayout->addWidget(hostFormPage_);
    devicePage_ = new DevicePage;
    contentLayout->addWidget(devicePage_);
    settingsPage_ = createSettingsPage();
    contentLayout->addWidget(settingsPage_);

    setTabOrder(hostEndButton_, settingsNavigation_);
    setTabOrder(settingsNavigation_, deviceNavigation_);
    setTabOrder(deviceNavigation_, assistanceNavigation_);
    setTabOrder(assistanceNavigation_, assistanceTabs_);
    setTabOrder(assistanceTabs_, invitation_);
    setTabOrder(invitation_, pasteButton_);
    setTabOrder(pasteButton_, joinButton_);
    setTabOrder(joinButton_, cancelButton_);
    setTabOrder(cancelButton_, invitationLifetime_);
    connect(assistanceNavigation_, &QPushButton::clicked, this, [this] { switchRole(hostPage_); });
    connect(assistanceTabs_, &QTabBar::currentChanged, this, [this](int index) { switchRole(index == 1, true); });
    connect(deviceNavigation_, &QPushButton::clicked, this, &HomeWindow::showDevicePage);
    connect(settingsNavigation_, &QPushButton::clicked, this, &HomeWindow::showSettingsPage);
    connect(devicePage_, &DevicePage::returnRequested, this, &HomeWindow::sessionActivationRequested);
    connect(hostEndButton_, &QPushButton::clicked, this, &HomeWindow::hostEndRequested);
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
    auto* form = new QFrame;
    hostForm_ = form;
    auto* formLayout = hostCard(form);
    hostSetup_ = new QWidget;
    auto* setupLayout = new QVBoxLayout(hostSetup_);
    setupLayout->setContentsMargins(0, 0, 0, 0);
    setupLayout->setSpacing(8);
    auto* fields = new QFormLayout;
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    fields->setRowWrapPolicy(QFormLayout::WrapLongRows);
    fields->setHorizontalSpacing(14);
    fields->setVerticalSpacing(8);
    setupLayout->addLayout(fields);
    auto* lifetimeLabel = label(QStringLiteral("邀请码有效期"), "fieldLabel", 13, QFont::DemiBold);
    invitationLifetime_ = native_ui::codeLifetimeSelector(lifetimeLabel, "invitationLifetime");
    invitationLifetime_->setMaximumWidth(200);
    fields->addRow(lifetimeLabel, invitationLifetime_);

    autoAcceptInvitation_ = new QCheckBox(QStringLiteral("持码免本机确认"));
    autoAcceptInvitation_->setObjectName(QStringLiteral("autoAcceptInvitation"));
    setupLayout->addWidget(autoAcceptInvitation_, 0, Qt::AlignLeft);
    auto* scope = label(QStringLiteral("仅对本次邀请码有效。开启后，首位使用此码的人可直接查看屏幕并控制本机。"),
                        "subtle", 13);
    scope->setWordWrap(true);
    autoAcceptInvitation_->setAccessibleDescription(scope->text());
    setupLayout->addWidget(scope);
    formLayout->addWidget(hostSetup_);

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
    invitationDetails_ = label({}, "invitationDetails", 13);
    invitationDetails_->setWordWrap(true);
    inviteLayout->addWidget(invitationDetails_);

    hostConsent_ = new QWidget;
    auto* consentLayout = new QVBoxLayout(hostConsent_);
    consentLayout->setContentsMargins(0, 0, 0, 0);
    consentLayout->addWidget(
        label(QStringLiteral("收到远程控制请求"), "fieldLabel", 16, QFont::DemiBold));
    auto* disclosure = label(
        QStringLiteral("使用此邀请码的设备请求连接。同意后，对方可以查看整个屏幕，并使用键盘和鼠标控制本机。"),
        "subtle", 14);
    disclosure->setWordWrap(true);
    consentLayout->addWidget(disclosure);
    formLayout->addWidget(hostConsent_);
    shareOptions_ = new QWidget;
    auto* optionsLayout = new QVBoxLayout(shareOptions_);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(4);
    shareAudio_ = new QCheckBox(QStringLiteral("共享系统声音"));
    shareAudio_->setObjectName(QStringLiteral("hostShareAudio"));
    shareAudio_->setChecked(true);
    highRefresh_ = new QCheckBox(QStringLiteral("原生分辨率，目标 144 帧/秒"));
    highRefresh_->setObjectName(QStringLiteral("hostHighRefresh"));
    relayOnly_ = new QCheckBox(QStringLiteral("仅使用 TURN 中继"));
    relayOnly_->setObjectName(QStringLiteral("hostRelayOnly"));
    for (auto* option : {shareAudio_, highRefresh_, relayOnly_})
        optionsLayout->addWidget(option, 0, Qt::AlignLeft);
    formLayout->addWidget(shareOptions_);
    hostCreate_ = hostButton(QStringLiteral("创建邀请码"), "hostCreate");
    formLayout->addWidget(hostCreate_, 0, Qt::AlignLeft);
    hostDecisions_ = new QWidget;
    auto* decisions = new QHBoxLayout(hostDecisions_);
    decisions->setContentsMargins(0, 0, 0, 0);
    decisions->setSpacing(8);
    auto* approve = hostButton(QStringLiteral("同意并开始共享"), "hostApprove");
    hostDeny_ = hostButton(QStringLiteral("拒绝"), "hostDeny");
    decisions->addWidget(approve);
    decisions->addWidget(hostDeny_);
    decisions->addStretch();
    formLayout->addWidget(hostDecisions_);
    pageLayout->addWidget(form);
    pageLayout->addWidget(invite);
    pageLayout->addStretch();

    setTabOrder(invitationLifetime_, autoAcceptInvitation_);
    setTabOrder(autoAcceptInvitation_, shareAudio_);
    setTabOrder(shareAudio_, highRefresh_);
    setTabOrder(hostInvitation_, copy);
    setTabOrder(highRefresh_, relayOnly_);
    setTabOrder(relayOnly_, hostCreate_);
    setTabOrder(hostCreate_, approve);
    setTabOrder(approve, hostDeny_);
    connect(autoAcceptInvitation_, &QCheckBox::toggled, this, &HomeWindow::updateForm);
    connect(hostCreate_, &QPushButton::clicked, this, &HomeWindow::requestHost);
    connect(copy, &QPushButton::clicked, this, &HomeWindow::copyHostInvitation);
    connect(approve, &QPushButton::clicked, this, [this] { respondToHostRequest(true); });
    connect(hostDeny_, &QPushButton::clicked, this, [this] { respondToHostRequest(false); });
    return page;
}

QWidget* HomeWindow::createSettingsPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(13);
    layout->addWidget(label(QStringLiteral("设置"), "title", 29, QFont::DemiBold));
    auto* card = new QFrame;
    auto* form = hostCard(card);
    settingsForm_ = card;
    layout->addWidget(card);
    form->addWidget(label(QStringLiteral("连接设置"), "sectionTitle", 17, QFont::DemiBold));
    auto* serverLabel = label(QStringLiteral("信令服务器"), "fieldLabel", 13, QFont::DemiBold);
    settingsServerUrl_ = new QLineEdit;
    settingsServerUrl_->setObjectName(QStringLiteral("settingsServerUrl"));
    settingsServerUrl_->setAccessibleName(serverLabel->text());
    settingsServerUrl_->setPlaceholderText(QStringLiteral("wss://服务器地址/"));
    settingsServerUrl_->setMinimumHeight(41);
    settingsServerUrl_->setMaxLength(2048);
    serverLabel->setBuddy(settingsServerUrl_);
    form->addWidget(serverLabel);
    form->addWidget(settingsServerUrl_);
    auto* keyLabel = label(QStringLiteral("部署密钥"), "fieldLabel", 13, QFont::DemiBold);
    settingsDeploymentKey_ = new QLineEdit;
    settingsDeploymentKey_->setObjectName(QStringLiteral("settingsDeploymentKey"));
    settingsDeploymentKey_->setAccessibleName(keyLabel->text());
    settingsDeploymentKey_->setMinimumHeight(41);
    settingsDeploymentKey_->setMaxLength(4096);
    settingsDeploymentKey_->setEchoMode(QLineEdit::Password);
    settingsDeploymentKey_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData |
                                               Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    keyLabel->setBuddy(settingsDeploymentKey_);
    form->addWidget(keyLabel);
    form->addWidget(settingsDeploymentKey_);
    forgetDeploymentKey_ = new QCheckBox(QStringLiteral("清除已保存的密钥"));
    forgetDeploymentKey_->setObjectName(QStringLiteral("forgetDeploymentKey"));
    form->addWidget(forgetDeploymentKey_);
    auto* save = hostButton(QStringLiteral("保存设置"), "saveSettings");
    form->addWidget(save, 0, Qt::AlignLeft);
    settingsBusyMessage_ = label(QStringLiteral("当前连接设置只读。"), "subtle", 13);
    settingsBusyMessage_->setWordWrap(true);
    layout->addWidget(settingsBusyMessage_);
    settingsMessage_ = label({}, "settingsMessage", 13);
    settingsMessage_->setWordWrap(true);
    layout->addWidget(settingsMessage_);
    settingsMessage_->hide();

    connect(settingsServerUrl_, &QLineEdit::textChanged, this, [this] {
        settingsDeploymentKey_->setText({});
        forgetDeploymentKey_->setChecked(false);
        updateKeyPlaceholder();
        setSettingsMessage({});
    });
    connect(forgetDeploymentKey_, &QCheckBox::toggled, this, [this](bool forget) {
        settingsDeploymentKey_->setText({});
        settingsDeploymentKey_->setEnabled(!forget);
    });
    connect(settingsServerUrl_, &QLineEdit::returnPressed, this, [this] {
        if (forgetDeploymentKey_->isChecked())
            forgetDeploymentKey_->setFocus();
        else
            settingsDeploymentKey_->setFocus();
    });
    connect(save, &QPushButton::clicked, this, &HomeWindow::saveSettings);
    setTabOrder(settingsServerUrl_, settingsDeploymentKey_);
    setTabOrder(settingsDeploymentKey_, forgetDeploymentKey_);
    setTabOrder(forgetDeploymentKey_, save);
    updateKeyPlaceholder();
    return page;
}

void HomeWindow::updateKeyPlaceholder()
{
    const QUrl draft(settingsServerUrl_->text().trimmed(), QUrl::StrictMode);
    const bool matchingKey = hasSavedKey_ && evp::deviceServerUrl(draft) == savedServerUrl_;
    settingsDeploymentKey_->setPlaceholderText(matchingKey ? QStringLiteral("已保存，留空保留现有密钥")
                                                          : QStringLiteral("未保存密钥，可留空使用邀请码连接"));
    forgetDeploymentKey_->setVisible(matchingKey);
}

void HomeWindow::resetSettingsDraft()
{
    settingsDeploymentKey_->setText({});
    forgetDeploymentKey_->setChecked(false);
    settingsServerUrl_->setText(savedServerUrl_.toString(QUrl::FullyEncoded));
    updateKeyPlaceholder();
}

void HomeWindow::setConnectionSettings(const QUrl& url, bool hasKey)
{
    const auto server = evp::deviceServerUrl(url);
    hasKey = hasKey && !server.isEmpty();
    savedServerUrl_ = server;
    hasSavedKey_ = hasKey;
    devicePage_->setConnectionConfigured(hasKey);
    resetSettingsDraft();
}

void HomeWindow::setConnectionSettingsEnabled(bool editable)
{
    settingsEditable_ = editable;
    updateSettingsAvailability();
}

void HomeWindow::updateSettingsAvailability()
{
    const bool enabled = settingsEditable_ && !busy_ && !hostBusy_;
    const bool moveFocus = !enabled && settingsForm_->isAncestorOf(focusWidget());
    settingsForm_->setEnabled(enabled);
    settingsBusyMessage_->setVisible(!enabled);
    if (moveFocus)
        settingsNavigation_->setFocus(Qt::OtherFocusReason);
}

void HomeWindow::setSettingsMessage(const QString& message)
{
    settingsMessage_->setText(message);
    settingsMessage_->setVisible(!message.isEmpty());
}

void HomeWindow::saveSettings()
{
    if (page_ != Page::Settings || !settingsForm_->isEnabled())
        return;
    const auto url = evp::deviceServerUrl(QUrl(settingsServerUrl_->text().trimmed(), QUrl::StrictMode));
    if (url.isEmpty()) {
        setSettingsMessage(QStringLiteral("请输入有效的 wss:// 信令服务器地址。"));
        settingsServerUrl_->setFocus();
        return;
    }
    const auto key = settingsDeploymentKey_->text();
    const bool forget = forgetDeploymentKey_->isChecked() && hasSavedKey_ && url == savedServerUrl_;
    emit settingsRequested(url, key, forget);
}

DevicePage& HomeWindow::devices()
{
    return *devicePage_;
}

void HomeWindow::showDevicePage()
{
    selectPage(Page::Devices);
}

void HomeWindow::showSettingsPage()
{
    selectPage(Page::Settings);
    if (settingsForm_->isEnabled())
        settingsServerUrl_->setFocus(Qt::ShortcutFocusReason);
    else
        settingsNavigation_->setFocus(Qt::ShortcutFocusReason);
}

void HomeWindow::selectPage(Page page)
{
    if (page_ == Page::Settings && page != page_)
        resetSettingsDraft();
    page_ = page;
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
        autoAcceptInvitation_->setChecked(false);
        if (trusted || page_ == Page::Settings)
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
    if (trustedSession_ && page_ != Page::Settings)
        showDevicePage();
    else {
        updateForm();
        updateBanner();
        if (page_ == Page::Assistance && !assistanceTabs_->hasFocus()) {
            if (busy)
                cancelButton_->setFocus(Qt::OtherFocusReason);
            else
                invitation_->setFocus(Qt::OtherFocusReason);
        }
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
    if (active && page_ == Page::Assistance && !assistanceTabs_->hasFocus())
        joinButton_->setFocus(Qt::OtherFocusReason);
}

bool HomeWindow::setHostBusy(bool busy, bool trusted)
{
    if (hostBusy_ == busy || (busy && busy_)) {
        return hostBusy_ == busy;
    }
    autoAcceptInvitation_->setChecked(false);
    if (busy) {
        const WId id = winId();
        if (!WTSRegisterSessionNotification(reinterpret_cast<HWND>(id), NOTIFY_FOR_THIS_SESSION))
            return false;
        notificationWindow_ = id;
        if (trusted || page_ == Page::Settings)
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
        issuedAutoApprove_ = false;
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
    if (trustedSession_ && page_ != Page::Settings)
        showDevicePage();
    else {
        updateForm();
        updateBanner();
        if (page_ == Page::Assistance && !assistanceTabs_->hasFocus()) {
            if (busy)
                hostEndButton_->setFocus(Qt::OtherFocusReason);
            else
                invitationLifetime_->setFocus(Qt::OtherFocusReason);
        }
    }
    return true;
}

void HomeWindow::setHostInvitation(const QString& invitation, int minutes, bool autoApprove)
{
    if (!hostBusy_ || hostRequestId_ || hostActive_) {
        return;
    }
    if (!invitation.isEmpty()) {
        issuedAutoApprove_ = autoApprove;
        invitationDetails_->setText(QStringLiteral("有效期 %1 · %2")
            .arg(evp::codeLifetimeLabel(minutes), autoApprove ? QStringLiteral("持码免本机确认")
                                                             : QStringLiteral("连接前须本机确认")));
    }
    hostInvitation_->setText(invitation);
    updateForm();
    if (!invitation.isEmpty() && page_ == Page::Assistance) {
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
        if (page_ == Page::Assistance) {
            hostDeny_->setFocus(Qt::OtherFocusReason);
            scroll_->ensureWidgetVisible(hostDecisions_, 0, 12);
        }
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
    if (page_ != Page::Settings)
        scroll_->ensureWidgetVisible(banner_, 0, 12);
}

void HomeWindow::switchRole(bool host, bool focusTab)
{
    if ((host && busy_) || (!host && hostBusy_)) {
        updateForm();
        return;
    }
    if (host != hostPage_) {
        hostPage_ = host;
        failed_ = false;
        statusMessage_ = host ? hostIdleMessage : idleMessage;
    }
    selectPage(Page::Assistance);
    if (focusTab) {
        assistanceTabs_->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (hostBusy_) {
        if (hostRequestId_) {
            hostDeny_->setFocus(Qt::ShortcutFocusReason);
            scroll_->ensureWidgetVisible(hostDecisions_, 0, 12);
        } else {
            hostEndButton_->setFocus(Qt::ShortcutFocusReason);
        }
        return;
    }
    QWidget* target = host ? static_cast<QWidget*>(invitationLifetime_) : invitation_;
    if (busy_) {
        target = sessionActive_ ? joinButton_ : cancelButton_;
    }
    target->setFocus(Qt::ShortcutFocusReason);
    scroll_->ensureWidgetVisible(target);
}

void HomeWindow::requestJoin()
{
    if (page_ != Page::Assistance || busy_ || hostBusy_ || hostPage_) {
        return;
    }
    if (savedServerUrl_.isEmpty()) {
        showSettingsPage();
        if (settingsForm_->isEnabled())
            setSettingsMessage(QStringLiteral("请先保存信令服务器地址，再返回远程协助使用邀请码连接。"));
        return;
    }

    const auto invitation = invitation_->text().trimmed();
    if (invitation.isEmpty()) {
        showFailure(QStringLiteral("请输入对方提供的邀请码。"));
        invitation_->setFocus();
        return;
    }
    if (invitation.startsWith(QStringLiteral("EVP1."), Qt::CaseInsensitive)) {
        showFailure(QStringLiteral("这是设备配对码，请前往「我的设备」输入并请求配对。"));
        invitation_->setFocus();
        return;
    }

    emit joinRequested(invitation);
}

void HomeWindow::requestHost()
{
    if (page_ != Page::Assistance || busy_ || hostBusy_ || !hostPage_) {
        return;
    }
    if (!hasSavedKey_) {
        showSettingsPage();
        if (settingsForm_->isEnabled()) {
            setSettingsMessage(QStringLiteral("创建邀请码需要保存信令服务器地址和部署密钥。"));
            if (!savedServerUrl_.isEmpty())
                settingsDeploymentKey_->setFocus(Qt::ShortcutFocusReason);
        }
        return;
    }
    const evp::CodeOptions options{invitationLifetime_->currentData().toInt(), autoAcceptInvitation_->isChecked()};
    autoAcceptInvitation_->setChecked(false);
    emit hostRequested(options, highRefresh_->isChecked(), relayOnly_->isChecked(), shareAudio_->isChecked());
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
    const bool assistance = page_ == Page::Assistance;
    const bool devices = page_ == Page::Devices;
    viewerFormPage_->setVisible(assistance && !hostPage_);
    hostFormPage_->setVisible(assistance && hostPage_);
    devicePage_->setVisible(devices);
    deviceNavigation_->setChecked(devices);
    assistanceNavigation_->setChecked(assistance);
    deviceTitle_->setVisible(devices);
    deviceSummary_->setVisible(devices);
    assistanceHeader_->setVisible(assistance);
    settingsPage_->setVisible(page_ == Page::Settings);
    settingsNavigation_->setChecked(page_ == Page::Settings);
    updateSettingsAvailability();
    {
        const QSignalBlocker blocker(assistanceTabs_);
        assistanceTabs_->setTabEnabled(0, !hostBusy_);
        assistanceTabs_->setTabEnabled(1, !busy_);
        assistanceTabs_->setCurrentIndex(hostPage_ ? 1 : 0);
    }
    devicePage_->setReturnVisible(sessionActive_);
    const bool pending = hostBusy_ && hostRequestId_;
    hostForm_->setVisible(!hostBusy_ || pending);
    hostSetup_->setVisible(!hostBusy_);
    hostCreate_->setVisible(!hostBusy_);
    hostInvite_->setVisible(hostBusy_ && !hostInvitation_->text().isEmpty());
    hostConsent_->setVisible(pending);
    hostDecisions_->setVisible(pending);
    shareOptions_->setVisible(pending || (!hostBusy_ && autoAcceptInvitation_->isChecked()));
    hostEndButton_->setVisible(hostBusy_);
    if (hostActive_ && (trustedSession_ || issuedAutoApprove_)) {
        if (!shareIndicator_->isVisible()) {
            shareIndicator_->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
            shareIndicator_->adjustSize();
            const auto area = screen()->availableGeometry();
            const auto margins = shareIndicator_->contentsMargins();
            shareIndicator_->move(area.right() + 1 - shareIndicator_->width() + margins.right() - 12,
                                  area.top() + 12 - margins.top());
            shareIndicator_->show();
        }
    } else {
        shareIndicator_->hide();
    }
    hostEndButton_->setText(hostActive_ ? QStringLiteral("结束共享")
                                      : QStringLiteral("取消共享"));
    invitation_->setEnabled(!busy_);
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
    banner_->setVisible(page_ == Page::Assistance || (page_ == Page::Devices && (hostBusy_ || busy_ || failed_)));
    bannerMessage_->setText(statusMessage_);
}
