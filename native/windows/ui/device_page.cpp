#include "device_page.h"
#include "visual_style.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

using native_ui::label;

QVBoxLayout* addCard(QVBoxLayout* page) {
    auto* card = new QFrame;
    card->setObjectName("card");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 13, 16, 13);
    layout->setSpacing(8);
    page->addWidget(card);
    return layout;
}

QPushButton* button(const QString& text, const char* name, bool primary = false) {
    auto* result = new QPushButton(text);
    result->setObjectName(name);
    result->setProperty("deviceAction", primary ? "primary" : "secondary");
    result->setMinimumHeight(34);
    result->setCursor(Qt::PointingHandCursor);
    return result;
}

QLineEdit* field(const QString& description, const char* name, bool secret = false) {
    auto* result = new QLineEdit;
    result->setObjectName(name);
    result->setAccessibleName(description);
    result->setPlaceholderText(description);
    result->setMinimumHeight(34);
    if (secret) {
        result->setEchoMode(QLineEdit::Password);
        result->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    }
    return result;
}

QHBoxLayout* row(QVBoxLayout* layout, QWidget* leading, QWidget* trailing, bool stretchLeading = true) {
    auto* result = new QHBoxLayout;
    result->setSpacing(8);
    result->addWidget(leading, stretchLeading ? 1 : 0);
    result->addWidget(trailing);
    layout->addLayout(result);
    return result;
}

}

DevicePage::DevicePage(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QStringLiteral(R"(
        QPushButton[deviceAction="primary"] { background: #2463da; color: white; border: 1px solid transparent; }
        QPushButton[deviceAction="primary"]:hover { background: #1956c5; }
        QPushButton[deviceAction="secondary"] { background: #f8faff; color: #284675; border: 1px solid #ccd9ec; }
        QPushButton[deviceAction="secondary"]:hover { background: #edf4ff; }
        QPushButton[deviceAction] { border-radius: 7px; padding: 0 10px; }
        QPushButton[deviceAction]:focus { border: 1px solid #3475e4; }
        QPushButton[deviceAction]:disabled { background: #f0f4fa; color: #667899; border: 1px solid #dfe6f1; }
        QListWidget#trustedDeviceList { background: white; border: 2px solid #e0e8f3; border-radius: 9px; }
        QListWidget#trustedDeviceList:focus { border-color: #3475e4; }
        QListWidget#trustedDeviceList::item {
            padding: 8px 12px; border: 1px solid transparent; border-radius: 7px;
            background: #f4f6fa; color: #263955;
        }
        QListWidget#trustedDeviceList::item:hover { background: #e8effa; border-color: #b8cbed; }
        QListWidget#trustedDeviceList::item:selected { background: #dceaff; border-color: #739bda; color: #163e7b; }
        QListWidget#trustedDeviceList::item:selected:hover { background: #cbdfff; border-color: #547fbe; }
        QListWidget#trustedDeviceList::item:focus { border: 1px solid #2459ab; }
    )"));
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(10);
    page->addWidget(label(QStringLiteral("我的设备"), "sectionTitle", 17, QFont::DemiBold));
    registration_ = label({}, "deviceRegistration", 13, QFont::DemiBold);
    registration_->setWordWrap(true);
    page->addWidget(registration_);

    auto* approval = addCard(page);
    pairApproval_ = approval->parentWidget();
    pairRequester_ = label({}, "pairRequester", 13, QFont::DemiBold);
    pairRequester_->setTextFormat(Qt::PlainText);
    pairRequester_->setWordWrap(true);
    pairRequester_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    approval->addWidget(pairRequester_);
    auto* allow = button(QStringLiteral("确认配对"), "approveDevicePairing", true);
    auto* deny = button(QStringLiteral("拒绝"), "denyDevicePairing");
    row(approval, allow, deny, false)->addStretch();
    pairApproval_->hide();

    auto* devices = addCard(page);
    list_ = new QListWidget;
    list_->setObjectName("trustedDeviceList");
    list_->setAccessibleName(QStringLiteral("可信设备列表"));
    list_->setMinimumHeight(110);
    list_->setMaximumHeight(180);
    list_->setSpacing(3);
    list_->setMouseTracking(true);
    list_->viewport()->setAttribute(Qt::WA_Hover);
    list_->setUniformItemSizes(true);
    list_->setTextElideMode(Qt::ElideMiddle);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* emptyLayout = new QVBoxLayout(list_->viewport());
    emptyList_ = label(QStringLiteral("尚未配对其他设备\n在下方生成或输入配对码，即可添加设备。"), "subtle", 13);
    emptyList_->setAlignment(Qt::AlignCenter);
    emptyList_->setWordWrap(true);
    emptyList_->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyLayout->addWidget(emptyList_);
    devices->addWidget(list_);
    connect_ = button(QStringLiteral("连接选中设备"), "connectDevice", true);
    remove_ = button(QStringLiteral("移除配对"), "removeDevice");
    auto* actions = row(devices, connect_, remove_, false);
    cancel_ = button(QStringLiteral("取消请求"), "cancelDeviceAttempt");
    return_ = button(QStringLiteral("返回控制窗口"), "returnDeviceSession", true);
    return_->hide();
    actions->addWidget(cancel_);
    actions->addWidget(return_);
    actions->addStretch();
    message_ = label({}, "deviceMessage", 13);
    message_->setTextFormat(Qt::PlainText);
    message_->setWordWrap(true);
    page->addWidget(message_);
    message_->hide();

    auto* pairing = addCard(page);
    pairing->addWidget(label(QStringLiteral("本机 ID 与设备配对"), "fieldLabel", 14, QFont::DemiBold));
    id_ = field(QStringLiteral("本机设备 ID"), "localDeviceId");
    id_->setReadOnly(true);
    copyId_ = button(QStringLiteral("复制 ID"), "copyDeviceId");
    row(pairing, id_, copyId_);
    auto* hint = label(QStringLiteral("在一台电脑生成配对码，在另一台粘贴。配对成功后，两台电脑会记住彼此。"), "subtle", 13);
    hint->setWordWrap(true);
    pairing->addWidget(hint);
    auto* lifetimeLabel = label(QStringLiteral("配对码有效期"), "fieldLabel", 13);
    pairingLifetime_ = new QComboBox;
    pairingLifetime_->setObjectName("pairingLifetime");
    pairingLifetime_->setAccessibleName(lifetimeLabel->text());
    lifetimeLabel->setBuddy(pairingLifetime_);
    for (int minutes : evp::codeLifetimeMinutes)
        pairingLifetime_->addItem(evp::codeLifetimeLabel(minutes), minutes);
    pairingLifetime_->setCurrentIndex(pairingLifetime_->findData(evp::CodeOptions{}.minutes));
    autoAcceptPairing_ = new QCheckBox(QStringLiteral("持码免本机确认"));
    autoAcceptPairing_->setObjectName("autoAcceptPairing");
    auto* issuance = row(pairing, lifetimeLabel, pairingLifetime_, false);
    issuance->addWidget(autoAcceptPairing_);
    issuance->addStretch();
    code_ = field(QStringLiteral("本机配对码"), "localPairingCode");
    code_->setReadOnly(true);
    makeCode_ = button(QStringLiteral("生成配对码"), "generatePairingCode");
    copyCode_ = button(QStringLiteral("复制"), "copyPairingCode");
    row(pairing, code_, makeCode_)->addWidget(copyCode_);
    issuedCodeDetails_ = label({}, "issuedPairingDetails", 13);
    issuedCodeDetails_->setWordWrap(true);
    issuedCodeDetails_->hide();
    pairing->addWidget(issuedCodeDetails_);
    peerCode_ = field(QStringLiteral("粘贴另一台设备的配对码"), "peerPairingCode", true);
    peerCode_->setMaxLength(128);
    pair_ = button(QStringLiteral("请求配对"), "pairDevice", true);
    row(pairing, peerCode_, pair_);

    auto* settings = addCard(page);
    settings->addWidget(label(QStringLiteral("远控权限与信令服务"), "fieldLabel", 14, QFont::DemiBold));
    incoming_ = new QCheckBox(QStringLiteral("允许可信设备远控本机（无需逐次确认）"));
    incoming_->setObjectName("allowTrustedControl");
    settings->addWidget(incoming_);
    auto* options = new QHBoxLayout;
    audio_ = new QCheckBox(QStringLiteral("共享系统声音"));
    audio_->setObjectName("deviceShareAudio");
    highRefresh_ = new QCheckBox(QStringLiteral("目标 144 帧/秒"));
    highRefresh_->setObjectName("deviceHighRefresh");
    relayOnly_ = new QCheckBox(QStringLiteral("仅使用 TURN 中继"));
    relayOnly_->setObjectName("deviceRelayOnly");
    for (auto* option : {audio_, highRefresh_, relayOnly_})
        options->addWidget(option);
    options->addStretch();
    settings->addLayout(options);
    url_ = field(QStringLiteral("信令服务器（wss://）"), "deviceServerUrl");
    url_->setMaxLength(2048);
    settings->addWidget(url_);
    key_ = field(QStringLiteral("部署密钥（仅首次设置或更改时需要）"), "deviceDeploymentKey", true);
    key_->setMaxLength(4096);
    configure_ = button(QStringLiteral("保存并连接"), "configureDevices");
    row(settings, key_, configure_);

    connect(configure_, &QPushButton::clicked, this, [this] {
        const auto secret = key_->text();
        key_->clear();
        emit configureRequested(QUrl(url_->text().trimmed(), QUrl::StrictMode), secret);
    });
    connect(makeCode_, &QPushButton::clicked, this, [this] {
        const bool preapprove = code_->text().isEmpty() && autoAcceptPairing_->isChecked();
        emit codeRequested({pairingLifetime_->currentData().toInt(), preapprove});
    });
    connect(pairingLifetime_, &QComboBox::currentIndexChanged, this, &DevicePage::pairingOptionsEdited);
    connect(autoAcceptPairing_, &QCheckBox::toggled, this, [this](bool checked) {
        emit pairingOptionsEdited();
        const QSignalBlocker blocker(autoAcceptPairing_);
        autoAcceptPairing_->setChecked(checked);
    });
    connect(copyId_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(id_->text()); });
    connect(copyCode_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(code_->text()); });
    connect(pair_, &QPushButton::clicked, this, [this] {
        const auto code = peerCode_->text().trimmed();
        peerCode_->clear();
        emit pairingRequested(code);
    });
    connect(peerCode_, &QLineEdit::textChanged, this, &DevicePage::updateActions);
    connect(peerCode_, &QLineEdit::returnPressed, pair_, &QPushButton::click);
    connect(allow, &QPushButton::clicked, this, [this] { emit pairingDecided(pendingApproval_, true); });
    connect(deny, &QPushButton::clicked, this, [this] { emit pairingDecided(pendingApproval_, false); });
    connect(list_, &QListWidget::itemSelectionChanged, this, &DevicePage::updateActions);
    connect(list_, &QListWidget::itemDoubleClicked, connect_, &QPushButton::click);
    connect(connect_, &QPushButton::clicked, this, [this] { emit connectRequested(selectedId()); });
    connect(remove_, &QPushButton::clicked, this, [this] { emit removeRequested(selectedId()); });
    connect(cancel_, &QPushButton::clicked, this, &DevicePage::cancelRequested);
    connect(return_, &QPushButton::clicked, this, &DevicePage::returnRequested);
    for (auto* option : {incoming_, audio_, highRefresh_, relayOnly_}) {
        connect(option, &QCheckBox::toggled, this, [this] {
            emit preferencesChanged(incoming_->isChecked(), audio_->isChecked(), highRefresh_->isChecked(),
                                    relayOnly_->isChecked());
        });
    }
    setDevices({}, {});
    setPreferences({}, false);
    setRegistration(false, QStringLiteral("配置信令服务器后即可添加设备。"));
}

void DevicePage::setIdentity(const QString& id) {
    id_->setText(id);
    updateActions();
}

void DevicePage::setServer(const QUrl& url) {
    url_->setText(url.toString(QUrl::FullyEncoded));
}

void DevicePage::setRegistration(bool online, const QString& status) {
    registered_ = online;
    setProperty("deviceRegistered", online);
    registration_->setText(status);
    updateActions();
}

void DevicePage::setActivity(bool sessionBusy, bool pending) {
    sessionBusy_ = sessionBusy;
    pending_ = pending;
    updateActions();
}

void DevicePage::setPreferences(const evp::DevicePreferences& options, bool editable) {
    const QSignalBlocker incomingBlock(incoming_), audioBlock(audio_), refreshBlock(highRefresh_), relayBlock(relayOnly_);
    preferencesEditable_ = editable;
    incoming_->setChecked(options.allowIncoming);
    audio_->setChecked(options.audio);
    highRefresh_->setChecked(options.highRefresh);
    relayOnly_->setChecked(options.relayOnly);
    updateActions();
}

void DevicePage::setDevices(const QMap<QString, evp::DevicePeer>& peers, const QSet<QString>& online) {
    const QSignalBlocker selection(list_);
    online_ = online;
    for (int index = list_->count() - 1; index >= 0; --index) {
        if (!peers.contains(list_->item(index)->data(Qt::UserRole).toString()))
            delete list_->takeItem(index);
    }
    int index = 0;
    for (auto it = peers.cbegin(); it != peers.cend(); ++it) {
        auto* item = list_->item(index);
        if (!item || item->data(Qt::UserRole).toString() != it.key()) {
            item = new QListWidgetItem;
            item->setData(Qt::UserRole, it.key());
            item->setToolTip(it.key());
            list_->insertItem(index, item);
        }
        const auto status = online_.contains(it.key()) ? QStringLiteral("在线") : QStringLiteral("离线");
        item->setText(QStringLiteral("%1   ·   %2").arg(it->name, status));
        ++index;
    }
    emptyList_->setVisible(peers.isEmpty());
    if (!peers.isEmpty() && !list_->currentItem())
        list_->setCurrentRow(0);
    updateActions();
}

void DevicePage::setPairingCode(const QString& code, evp::CodeOptions options) {
    const auto old = code_->text();
    if (!old.isEmpty() && QApplication::clipboard()->text() == old)
        QApplication::clipboard()->clear();
    code_->setText(code);
    if (!old.isEmpty() || !code.isEmpty()) {
        const QSignalBlocker blocker(autoAcceptPairing_);
        autoAcceptPairing_->setChecked(!code.isEmpty() && options.autoApprove);
    }
    const bool preapproved = !code.isEmpty() && options.autoApprove;
    makeCode_->setText(preapproved ? QStringLiteral("重新生成（需确认）") : QStringLiteral("生成配对码"));
    issuedCodeDetails_->setText(code.isEmpty() ? QString{} :
        QStringLiteral("此码有效期 %1，限用一次。%2").arg(evp::codeLifetimeLabel(options.minutes),
            preapproved ? QStringLiteral("持码免本机确认，取消勾选即可使此码失效。")
                        : QStringLiteral("请在本机核对并确认请求。")));
    issuedCodeDetails_->setVisible(!code.isEmpty());
    updateActions();
}

void DevicePage::setPairingRequest(const QString& attempt, const QString& name, const QString& id) {
    pendingApproval_ = attempt;
    pairRequester_->setText(attempt.isEmpty() ? QString{} :
        QStringLiteral("「%1」请求建立长期配对。请确认这是你的设备。\n设备 ID：%2").arg(name, id));
    pairApproval_->setVisible(!attempt.isEmpty());
}

void DevicePage::setMessage(const QString& text) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty());
}

void DevicePage::setReturnVisible(bool visible) {
    return_->setVisible(visible);
}

QString DevicePage::selectedId() const {
    const auto selected = list_->selectedItems();
    return selected.isEmpty() ? QString{} : selected.first()->data(Qt::UserRole).toString();
}

void DevicePage::updateActions() {
    const bool idle = !sessionBusy_ && !pending_;
    const auto selected = selectedId();
    connect_->setEnabled(idle && registered_ && online_.contains(selected));
    remove_->setEnabled(!selected.isEmpty());
    makeCode_->setEnabled(idle && registered_);
    pairingLifetime_->setEnabled(makeCode_->isEnabled());
    autoAcceptPairing_->setEnabled(makeCode_->isEnabled());
    copyCode_->setEnabled(idle && !code_->text().isEmpty());
    copyId_->setEnabled(!id_->text().isEmpty());
    peerCode_->setEnabled(idle && registered_);
    pair_->setEnabled(peerCode_->isEnabled() && !peerCode_->text().trimmed().isEmpty());
    cancel_->setVisible(pending_ && !sessionBusy_);
    url_->setEnabled(idle && !id_->text().isEmpty());
    key_->setEnabled(url_->isEnabled());
    configure_->setEnabled(url_->isEnabled());
    incoming_->setEnabled(preferencesEditable_ && (idle || incoming_->isChecked()));
    for (auto* option : {audio_, highRefresh_, relayOnly_})
        option->setEnabled(preferencesEditable_ && idle);
}
