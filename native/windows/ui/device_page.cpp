#include "device_page.h"
#include "action_button.h"
#include "selection_buttons.h"
#include "select_field.h"
#include "theme.h"
#include "visual_style.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QToolTip>
#include <QVBoxLayout>

namespace {

using native_ui::label;

constexpr int DeviceNameRole = Qt::UserRole + 1;
constexpr int DevicePinnedRole = Qt::UserRole + 2;

QString itemId(const QListWidgetItem* item) {
    return item ? item->data(Qt::UserRole).toString() : QString{};
}

QString pinActionText(bool pinned) {
    return pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶");
}

QRect pinArea(const QRect& row) {
    constexpr int extent = 28;
    constexpr int rightInset = 8;
    const int height = qMin(row.height(), extent);
    return {row.right() - rightInset - extent + 1, row.top() + (row.height() - height) / 2, extent, height};
}

class DeviceItemDelegate final : public QStyledItemDelegate {
public:
    DeviceItemDelegate(const std::optional<QPoint>& hoverPosition, QObject* parent)
        : QStyledItemDelegate(parent), hoverPosition_(hoverPosition) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        // The list view expands vertical rows to the viewport width.
        return {1, QStyledItemDelegate::sizeHint(option, index).height()};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const bool hovered = hoverPosition_ && option.rect.contains(*hoverPosition_);
        auto row = option;
        row.state.setFlag(QStyle::State_MouseOver, hovered);
        QStyledItemDelegate::paint(painter, row, index);
        const bool pinned = index.data(DevicePinnedRole).toBool();
        if (!pinned && !hovered)
            return;

        const auto ink = pinned ? native_ui::themeColor("#2459ab", "#a9c7ff", "accentText")
                                : native_ui::themeColor("#4a6288", "#aebdce", "muted");
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(QRectF(pinArea(option.rect)).center());
        painter->rotate(32);
        painter->setPen(QPen(ink, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawLine(QPointF(0, 3), QPointF(0, 8));
        painter->setBrush(pinned ? QBrush(ink) : QBrush(Qt::NoBrush));
        painter->drawPolygon(QPolygonF{{-3.1, -7}, {3.1, -7}, {2.4, -2.5}, {4.7, 0.3},
                                       {4.7, 3}, {-4.7, 3}, {-4.7, 0.3}, {-2.4, -2.5}});
        painter->restore();
    }

private:
    const std::optional<QPoint>& hoverPosition_;
};

QVBoxLayout* addCard(QVBoxLayout* page) {
    auto* card = new QFrame;
    card->setObjectName("card");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 13, 16, 13);
    layout->setSpacing(8);
    page->addWidget(card);
    return layout;
}

native_ui::ActionButton* button(const QString& text, const char* name, bool primary = false) {
    auto* result = new native_ui::ActionButton(text);
    result->setObjectName(name);
    result->setProperty("deviceAction", primary ? "primary" : "secondary");
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
    native_ui::setThemedStyle(*this, QStringLiteral(R"(
        QPushButton[deviceAction="primary"] { background: #2463da/#2355ad@accent; color: white/#eef0f5@onAccent; border: 1px solid transparent; }
        QPushButton[deviceAction="primary"]:hover { background: #1956c5/#2a60bc@accentHover; }
        QPushButton[deviceAction="secondary"] { background: #f8faff/#222427@surface; color: #284675/#b7d0ff@accentText; border: 1px solid #ccd9ec/#3d434d@stroke; }
        QPushButton[deviceAction="secondary"]:hover { background: #edf4ff/#303844@raised; }
        QPushButton[deviceAction] { padding: 0 10px; }
        QPushButton[deviceAction]:focus { border: 1px solid #3475e4/#739ff1@accentMark; }
        QPushButton[deviceAction="primary"]:focus { border-color: #8db5ff/#a2c0ff@onAccent; }
        QPushButton[deviceAction]:disabled { background: #f0f4fa/#24262a@surface; color: #667899/#9da7b8@muted; border: 1px solid #dfe6f1/#343942@stroke; }
        QListWidget#trustedDeviceList { background: white/#25272b@surface; border: 2px solid #e0e8f3/#343942@stroke; border-radius: 9px; }
        QListWidget#trustedDeviceList:focus { border-color: #3475e4/#739ff1@accentMark; }
        QListWidget#trustedDeviceList::item {
            padding: 8px 42px 8px 12px; border: 1px solid transparent; border-radius: 7px;
            background: #f4f6fa/#222427@surface; color: #263955/#e6e8ec@foreground;
        }
        QListWidget#trustedDeviceList::item:hover { background: #e8effa/#303844@raised; border-color: #b8cbed/#536783@stroke; }
        QListWidget#trustedDeviceList::item:selected { background: #dceaff/#283b56@accentTint; border-color: #739bda/#627eac@accentMark; color: #163e7b/#b7d0ff@accentText; }
        QListWidget#trustedDeviceList::item:selected:hover { background: #cbdfff/#304867@accentHoverTint; border-color: #547fbe/#739ff1@accentMark; }
        QListWidget#trustedDeviceList::item:focus { border: 1px solid #2459ab/#739ff1@accentMark; }
    )"));
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(10);
    page->addWidget(label(QStringLiteral("我的设备"), "sectionTitle", 17, QFont::DemiBold));
    registration_ = label({}, "deviceRegistration", 13, QFont::DemiBold);
    registration_->setWordWrap(true);
    page->addWidget(registration_);
    settingsHint_ = label(QStringLiteral("请在左下角「设置」中保存服务器地址和部署密钥，以使用设备配对。"), "subtle", 13);
    settingsHint_->setWordWrap(true);
    page->addWidget(settingsHint_);

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
    list_->setItemDelegate(new DeviceItemDelegate(pinHoverPosition_, list_));
    list_->setResizeMode(QListView::Adjust);
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

    deviceMenu_ = new QMenu(this);
    deviceMenu_->setObjectName("trustedDeviceMenu");
    deviceMenu_->setAttribute(Qt::WA_TranslucentBackground);
    deviceMenu_->setFont(native_ui::uiFont(14));
    deviceMenu_->setMinimumWidth(144);
    native_ui::setThemedStyle(*deviceMenu_, QStringLiteral(R"(
        QMenu { background: #ffffff/#25272b@surface; color: #263955/#e6e8ec@foreground;
                border: 1px solid #cbd7e8/#495160@stroke; border-radius: 9px; padding: 5px; }
        QMenu::item { padding: 7px 18px 7px 14px; border-radius: 5px; }
        QMenu::icon { left: 5px; }
        QMenu::item:selected { background: #e8effa/#344153@accentHoverTint; color: #214c98/#b7d0ff@accentText; }
        QMenu::item:disabled { color: #778598/#929baa@muted; background: transparent; }
        QMenu::separator { height: 1px; margin: 4px 8px; background: #dce5f1/#424953@stroke; }
    )"));
    auto* rename = deviceMenu_->addAction(QStringLiteral("重命名"));
    rename->setObjectName("renameDeviceAction");
    moveUp_ = deviceMenu_->addAction(QStringLiteral("上移"));
    moveUp_->setObjectName("moveDeviceUpAction");
    moveDown_ = deviceMenu_->addAction(QStringLiteral("下移"));
    moveDown_->setObjectName("moveDeviceDownAction");
    pin_ = deviceMenu_->addAction(pinActionText(false));
    pin_->setObjectName("moveDeviceTopAction");
    deviceMenu_->addSeparator();
    auto* removeDevice = deviceMenu_->addAction(QStringLiteral("移除"));
    removeDevice->setObjectName("removeDeviceAction");
    connect(deviceMenu_, &QMenu::aboutToShow, this, [this, rename, removeDevice] {
        using native_ui::Symbol;
        const auto ink = native_ui::themeColor("#456594", "#aec9fb", "accentText");
        const auto icon = [&](Symbol glyph) { return native_ui::symbolIcon(glyph, ink, 16); };
        rename->setIcon(icon(Symbol::Rename));
        moveUp_->setIcon(icon(Symbol::MoveUp));
        moveDown_->setIcon(icon(Symbol::MoveDown));
        removeDevice->setIcon(native_ui::symbolIcon(Symbol::Remove, native_ui::themeColor("#ac4037", "#edaba7"), 16));
    });
    connect(rename, &QAction::triggered, this, &DevicePage::renameContextDevice);
    connect(moveUp_, &QAction::triggered, this, [this] { moveContextDevice(evp::DeviceMove::Up); });
    connect(moveDown_, &QAction::triggered, this, [this] { moveContextDevice(evp::DeviceMove::Down); });
    connect(pin_, &QAction::triggered, this, [this] { toggleDevicePin(takeContextDeviceId()); });
    connect(removeDevice, &QAction::triggered, this, [this] {
        const auto id = takeContextDeviceId();
        if (deviceItem(id))
            emit removeRequested(id);
    });
    list_->installEventFilter(this);
    list_->viewport()->installEventFilter(this);
    connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (pinHoverPosition_)
            refreshPinHover();
    }, Qt::QueuedConnection);

    renameDialog_ = new QInputDialog(this);
    renameDialog_->setObjectName("renameDeviceDialog");
    renameDialog_->setWindowTitle(QStringLiteral("重命名设备"));
    renameDialog_->setInputMode(QInputDialog::TextInput);
    renameDialog_->setLabelText(QStringLiteral("设备名称"));
    renameDialog_->setOkButtonText(QStringLiteral("确定"));
    renameDialog_->setCancelButtonText(QStringLiteral("取消"));
    auto* nameEdit = renameDialog_->findChild<QLineEdit*>();
    nameEdit->setMaxLength(evp::maxDeviceNameLength);
    auto* confirmName = renameDialog_->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    confirmName->setEnabled(false);
    connect(nameEdit, &QLineEdit::textChanged, confirmName, [confirmName](const QString& text) {
        confirmName->setEnabled(!evp::normalizedDeviceName(text).isEmpty());
    });
    connect(renameDialog_, &QDialog::accepted, this, [this] {
        const auto id = renameId_;
        renameId_.clear();
        const auto name = evp::normalizedDeviceName(renameDialog_->textValue());
        if (deviceItem(id) && !name.isEmpty())
            emit renameRequested(id, name);
    });
    connect(renameDialog_, &QDialog::rejected, this, [this] { renameId_.clear(); });

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
    pairingLifetime_ = native_ui::codeLifetimeSelector(lifetimeLabel, "pairingLifetime");
    autoAcceptPairing_ = new native_ui::CheckBox(QStringLiteral("持码免本机确认"));
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
    settings->addWidget(label(QStringLiteral("远控权限"), "fieldLabel", 14, QFont::DemiBold));
    incoming_ = new native_ui::CheckBox(QStringLiteral("允许可信设备远控本机（无需逐次确认）"));
    incoming_->setObjectName("allowTrustedControl");
    settings->addWidget(incoming_, 0, Qt::AlignLeft);
    auto* options = new QHBoxLayout;
    audio_ = new native_ui::CheckBox(QStringLiteral("共享系统声音"));
    audio_->setObjectName("deviceShareAudio");
    relayOnly_ = new native_ui::CheckBox(QStringLiteral("仅使用 TURN 中继"));
    relayOnly_->setObjectName("deviceRelayOnly");
    for (auto* option : {audio_, relayOnly_})
        options->addWidget(option);
    options->addStretch();
    settings->addLayout(options);
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
    for (auto* option : {incoming_, audio_, relayOnly_}) {
        connect(option, &QCheckBox::toggled, this, [this] {
            emit preferencesChanged(incoming_->isChecked(), audio_->isChecked(), relayOnly_->isChecked());
        });
    }
    setDevices({}, {});
    setPreferences({}, false);
    setRegistration(false, QStringLiteral("本机尚未上线"));
}

void DevicePage::setIdentity(const QString& id) {
    id_->setText(id);
    updateActions();
}

void DevicePage::setConnectionConfigured(bool configured) {
    settingsHint_->setVisible(!configured);
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
    const QSignalBlocker incomingBlock(incoming_), audioBlock(audio_), relayBlock(relayOnly_);
    preferencesEditable_ = editable;
    incoming_->setChecked(options.allowIncoming);
    audio_->setChecked(options.audio);
    relayOnly_->setChecked(options.relayOnly);
    updateActions();
}

void DevicePage::setDevices(const QMap<QString, evp::DevicePeer>& peers, const QSet<QString>& online) {
    const QSignalBlocker selection(list_);
    const auto selected = selectedId();
    const bool removedSelection = !selected.isEmpty() && !peers.contains(selected);
    auto* scrollBar = list_->verticalScrollBar();
    const int scrollPosition = scrollBar->value();
    online_ = online;
    QMap<QString, QListWidgetItem*> existingItems;
    for (int index = list_->count() - 1; index >= 0; --index) {
        auto* item = list_->item(index);
        const auto id = itemId(item);
        if (peers.contains(id))
            existingItems.insert(id, item);
        else
            delete list_->takeItem(index);
    }
    int index = 0;
    for (const auto& id : evp::orderedDeviceIds(peers)) {
        auto* item = existingItems.value(id);
        if (!item) {
            item = new QListWidgetItem;
            item->setData(Qt::UserRole, id);
            item->setToolTip(id);
            list_->insertItem(index, item);
        } else if (list_->item(index) != item) {
            list_->model()->moveRow({}, list_->row(item), {}, index);
        }
        const auto peer = peers.constFind(id);
        const auto name = peer->displayName();
        const auto status = online_.contains(id) ? QStringLiteral("在线") : QStringLiteral("离线");
        item->setData(DeviceNameRole, name);
        item->setData(DevicePinnedRole, peer->pinned);
        item->setText(QStringLiteral("%1   ·   %2").arg(name, status));
        ++index;
    }
    emptyList_->setVisible(peers.isEmpty());
    if (!peers.isEmpty() && (!list_->currentItem() || (removedSelection && selectedId().isEmpty())))
        list_->setCurrentRow(qMax(0, list_->currentRow()));
    if (scrollBar->value() != scrollPosition) {
        list_->doItemsLayout();
        scrollBar->setValue(scrollPosition);
    }
    if (!contextId_.isEmpty())
        updateDeviceMenu();
    if (!renameId_.isEmpty() && !peers.contains(renameId_))
        renameDialog_->reject();
    if (!pressedPinId_.isEmpty() && !peers.contains(pressedPinId_))
        pressedPinId_.clear();
    if (pinHoverPosition_)
        refreshPinHover();
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

void DevicePage::clearMessageIf(const QString& text) {
    if (message_->text() == text)
        setMessage({});
}

void DevicePage::setReturnVisible(bool visible) {
    return_->setVisible(visible);
}

bool DevicePage::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_->viewport() && handlePinEvent(event))
        return true;
    if (event->type() == QEvent::ContextMenu) {
        const auto* menu = static_cast<QContextMenuEvent*>(event);
        if (menu->reason() == QContextMenuEvent::Keyboard) {
            showCurrentDeviceMenu();
        } else {
            auto* viewport = list_->viewport();
            const auto position = watched == list_ ? viewport->mapFrom(list_, menu->pos()) : menu->pos();
            if (viewport->rect().contains(position)) {
                if (auto* item = list_->itemAt(position))
                    showDeviceMenu(item, menu->globalPos());
            }
        }
        return true;
    }
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier)) {
            showCurrentDeviceMenu();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QString DevicePage::selectedId() const {
    const auto selected = list_->selectedItems();
    return selected.isEmpty() ? QString{} : itemId(selected.first());
}

QListWidgetItem* DevicePage::deviceItem(const QString& id) const {
    if (id.isEmpty())
        return nullptr;
    for (int row = 0; row < list_->count(); ++row) {
        auto* item = list_->item(row);
        if (itemId(item) == id)
            return item;
    }
    return nullptr;
}

QListWidgetItem* DevicePage::pinItemAt(const QPoint& position) const {
    if (!list_->viewport()->rect().contains(position))
        return nullptr;
    auto* item = list_->itemAt(position);
    return item && pinArea(list_->visualItemRect(item)).contains(position) ? item : nullptr;
}

bool DevicePage::handlePinEvent(QEvent* event) {
    switch (event->type()) {
    case QEvent::Enter:
        setPinHoverPosition(static_cast<QEnterEvent*>(event)->position().toPoint());
        break;
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
        setPinHoverPosition(static_cast<QHoverEvent*>(event)->position().toPoint());
        break;
    case QEvent::MouseMove:
        setPinHoverPosition(static_cast<QMouseEvent*>(event)->position().toPoint());
        break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
        setPinHoverPosition(std::nullopt);
        break;
    case QEvent::ToolTip: {
        const auto* help = static_cast<QHelpEvent*>(event);
        if (const auto* item = pinItemAt(help->pos())) {
            QToolTip::showText(help->globalPos(), pinActionText(item->data(DevicePinnedRole).toBool()),
                               list_->viewport(), pinArea(list_->visualItemRect(item)));
            return true;
        }
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            break;
        const auto id = itemId(pinItemAt(mouse->position().toPoint()));
        if (event->type() == QEvent::MouseButtonDblClick) {
            pinMouseGesture_ = previousPressWasPin_ || !id.isEmpty();
            previousPressWasPin_ = false;
            pressedPinId_.clear();
        } else {
            pinMouseGesture_ = !id.isEmpty();
            previousPressWasPin_ = pinMouseGesture_;
            pressedPinId_ = id;
        }
        return pinMouseGesture_;
    }
    case QEvent::MouseButtonRelease: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton || !pinMouseGesture_)
            break;
        const auto id = pressedPinId_;
        pressedPinId_.clear();
        pinMouseGesture_ = false;
        if (!id.isEmpty() && itemId(pinItemAt(mouse->position().toPoint())) == id)
            toggleDevicePin(id);
        return true;
    }
    default:
        break;
    }
    return false;
}

void DevicePage::setPinHoverPosition(std::optional<QPoint> position) {
    if (position && !list_->viewport()->rect().contains(*position))
        position.reset();
    if (pinHoverPosition_ != position) {
        pinHoverPosition_ = position;
        refreshPinHover();
    }
}

void DevicePage::refreshPinHover() {
    auto* viewport = list_->viewport();
    viewport->update();
    if (pinHoverPosition_ && pinItemAt(*pinHoverPosition_)) {
        if (viewport->cursor().shape() != Qt::PointingHandCursor)
            viewport->setCursor(Qt::PointingHandCursor);
    } else if (viewport->testAttribute(Qt::WA_SetCursor)) {
        viewport->unsetCursor();
    }
}

void DevicePage::toggleDevicePin(const QString& id) {
    const auto* item = deviceItem(id);
    if (!item)
        return;
    const bool pinned = !item->data(DevicePinnedRole).toBool();
    emit pinRequested(id, pinned);
    if (auto* moved = deviceItem(id); moved && moved->data(DevicePinnedRole).toBool() == pinned) {
        list_->setCurrentItem(moved);
        list_->scrollToItem(moved);
    }
}

void DevicePage::showCurrentDeviceMenu() {
    if (auto* item = list_->currentItem()) {
        list_->scrollToItem(item);
        const auto position = list_->visualItemRect(item).center();
        showDeviceMenu(item, list_->viewport()->mapToGlobal(position));
    }
}

void DevicePage::showDeviceMenu(QListWidgetItem* item, const QPoint& globalPosition) {
    contextId_ = itemId(item);
    list_->setCurrentItem(item);
    updateDeviceMenu();
    if (!contextId_.isEmpty())
        deviceMenu_->popup(globalPosition);
}

void DevicePage::updateDeviceMenu() {
    const auto* item = deviceItem(contextId_);
    if (!item) {
        contextId_.clear();
        deviceMenu_->close();
        return;
    }
    moveUp_->setEnabled(canMoveDevice(item, evp::DeviceMove::Up));
    moveDown_->setEnabled(canMoveDevice(item, evp::DeviceMove::Down));
    const bool pinned = item->data(DevicePinnedRole).toBool();
    pin_->setText(pinActionText(pinned));
    pin_->setIcon(native_ui::symbolIcon(pinned ? native_ui::Symbol::Unpin : native_ui::Symbol::Pin,
                                       native_ui::themeColor("#456594", "#aec9fb", "accentText"), 16));
}

QString DevicePage::takeContextDeviceId() {
    const auto id = contextId_;
    contextId_.clear();
    deviceMenu_->close();
    return id;
}

bool DevicePage::canMoveDevice(const QListWidgetItem* item, evp::DeviceMove move) const {
    if (!item)
        return false;
    const int adjacentRow = list_->row(item) + (move == evp::DeviceMove::Up ? -1 : 1);
    const auto* adjacentItem = list_->item(adjacentRow);
    return adjacentItem && adjacentItem->data(DevicePinnedRole) == item->data(DevicePinnedRole);
}

void DevicePage::renameContextDevice() {
    const auto id = takeContextDeviceId();
    if (const auto* item = deviceItem(id)) {
        renameId_ = id;
        renameDialog_->setTextValue(item->data(DeviceNameRole).toString());
        renameDialog_->open();
    }
}

void DevicePage::moveContextDevice(evp::DeviceMove move) {
    const auto id = takeContextDeviceId();
    if (canMoveDevice(deviceItem(id), move))
        emit moveRequested(id, move);
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
    incoming_->setEnabled(preferencesEditable_ && (idle || incoming_->isChecked()));
    for (auto* option : {audio_, relayOnly_})
        option->setEnabled(preferencesEditable_ && idle);
}
