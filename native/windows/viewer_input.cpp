#include "viewer_input.h"
#include "media/video_surface.h"
#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <array>
#include <optional>
#include <utility>

namespace evp {
namespace {
struct Key {
    int key;
    const char* code;
};
constexpr Key named[] = {{Qt::Key_Escape, "Escape"},     {Qt::Key_Backspace, "Backspace"},
                         {Qt::Key_Tab, "Tab"},           {Qt::Key_Backtab, "Tab"},
                         {Qt::Key_Return, "Enter"},      {Qt::Key_Enter, "NumpadEnter"},
                         {Qt::Key_Space, "Space"},       {Qt::Key_Left, "ArrowLeft"},
                         {Qt::Key_Right, "ArrowRight"},  {Qt::Key_Up, "ArrowUp"},
                         {Qt::Key_Down, "ArrowDown"},    {Qt::Key_Delete, "Delete"},
                         {Qt::Key_Insert, "Insert"},     {Qt::Key_Home, "Home"},
                         {Qt::Key_End, "End"},           {Qt::Key_PageUp, "PageUp"},
                         {Qt::Key_PageDown, "PageDown"}, {Qt::Key_CapsLock, "CapsLock"},
                         {Qt::Key_NumLock, "NumLock"},   {Qt::Key_ScrollLock, "ScrollLock"},
                         {Qt::Key_Menu, "ContextMenu"}};

QString physicalCode(const QKeyEvent& event) {
    // Windows set-1 make codes preserve the physical key across keyboard layouts.
    const unsigned scan = event.nativeScanCode();
    const unsigned low = scan & 0xff;
    const bool extended = (scan & 0xff00) != 0;
    if (extended) {
        switch (low) {
        case 0x1c:
            return "NumpadEnter";
        case 0x1d:
            return "ControlRight";
        case 0x35:
            return "NumpadDivide";
        case 0x37:
            return "PrintScreen";
        case 0x38:
            return "AltRight";
        case 0x47:
            return "Home";
        case 0x48:
            return "ArrowUp";
        case 0x49:
            return "PageUp";
        case 0x4b:
            return "ArrowLeft";
        case 0x4d:
            return "ArrowRight";
        case 0x4f:
            return "End";
        case 0x50:
            return "ArrowDown";
        case 0x51:
            return "PageDown";
        case 0x52:
            return "Insert";
        case 0x53:
            return "Delete";
        case 0x5b:
            return "MetaLeft";
        case 0x5c:
            return "MetaRight";
        case 0x5d:
            return "ContextMenu";
        }
    }
    static constexpr const char* codes[] = {"",
                                            "Escape",
                                            "Digit1",
                                            "Digit2",
                                            "Digit3",
                                            "Digit4",
                                            "Digit5",
                                            "Digit6",
                                            "Digit7",
                                            "Digit8",
                                            "Digit9",
                                            "Digit0",
                                            "Minus",
                                            "Equal",
                                            "Backspace",
                                            "Tab",
                                            "KeyQ",
                                            "KeyW",
                                            "KeyE",
                                            "KeyR",
                                            "KeyT",
                                            "KeyY",
                                            "KeyU",
                                            "KeyI",
                                            "KeyO",
                                            "KeyP",
                                            "BracketLeft",
                                            "BracketRight",
                                            "Enter",
                                            "ControlLeft",
                                            "KeyA",
                                            "KeyS",
                                            "KeyD",
                                            "KeyF",
                                            "KeyG",
                                            "KeyH",
                                            "KeyJ",
                                            "KeyK",
                                            "KeyL",
                                            "Semicolon",
                                            "Quote",
                                            "Backquote",
                                            "ShiftLeft",
                                            "Backslash",
                                            "KeyZ",
                                            "KeyX",
                                            "KeyC",
                                            "KeyV",
                                            "KeyB",
                                            "KeyN",
                                            "KeyM",
                                            "Comma",
                                            "Period",
                                            "Slash",
                                            "ShiftRight",
                                            "NumpadMultiply",
                                            "AltLeft",
                                            "Space",
                                            "CapsLock",
                                            "F1",
                                            "F2",
                                            "F3",
                                            "F4",
                                            "F5",
                                            "F6",
                                            "F7",
                                            "F8",
                                            "F9",
                                            "F10",
                                            "NumLock",
                                            "ScrollLock",
                                            "Numpad7",
                                            "Numpad8",
                                            "Numpad9",
                                            "NumpadSubtract",
                                            "Numpad4",
                                            "Numpad5",
                                            "Numpad6",
                                            "NumpadAdd",
                                            "Numpad1",
                                            "Numpad2",
                                            "Numpad3",
                                            "Numpad0",
                                            "NumpadDecimal"};
    if (low && low < std::size(codes))
        return QString::fromLatin1(codes[low]);
    if (low == 0x56)
        return "IntlBackslash";
    if (event.key() >= Qt::Key_A && event.key() <= Qt::Key_Z)
        return "Key" + QString(QChar(event.key()));
    if (event.key() >= Qt::Key_0 && event.key() <= Qt::Key_9)
        return "Digit" + QString(QChar(event.key()));
    if (event.key() >= Qt::Key_F1 && event.key() <= Qt::Key_F12)
        return "F" + QString::number(event.key() - Qt::Key_F1 + 1);
    for (auto entry : named)
        if (entry.key == event.key())
            return QString::fromLatin1(entry.code);
    return {};
}
std::optional<int> mouseButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton:
        return 0;
    case Qt::MiddleButton:
        return 1;
    case Qt::RightButton:
        return 2;
    default:
        return {};
    }
}
} // namespace
ViewerInput::ViewerInput(VideoSurface* surface, Sender sender, QObject* parent)
    : QObject(parent), surface_(surface), send_(std::move(sender)) {
    surface_->setFocusPolicy(Qt::StrongFocus);
    surface_->setMouseTracking(true);
    optimisticCursor_.setSingleShot(true);
    optimisticCursor_.setTimerType(Qt::PreciseTimer);
    optimisticCursor_.setInterval(150);
    connect(&optimisticCursor_, &QTimer::timeout, this, [this] {
        cursor_.expirePrediction();
        updateCursor();
    });
    qApp->installEventFilter(this);
}
ViewerInput::~ViewerInput() {
    qApp->removeEventFilter(this);
    resetCursor();
}
void ViewerInput::setAvailable(bool available) {
    const bool previouslyAvailable = available_;
    available_ = available;
    if (!available && previouslyAvailable)
        release();
    updateCursor();
}
void ViewerInput::release() {
    if (!keys_.isEmpty() || !buttons_.isEmpty())
        send_({{"type", "release"}});
    keys_.clear();
    buttons_.clear();
    optimisticCursor_.stop();
    cursor_.expirePrediction();
    updateCursor();
}

void ViewerInput::receiveCursor(const CursorPacket& packet) {
    if (cursor_.receive(packet)) updateCursor();
}

void ViewerInput::resetCursor() {
    optimisticCursor_.stop();
    cursor_ = {};
    updateCursor();
}

void ViewerInput::updateCursor() {
    const auto content = surface_->contentRect();
    const bool inside = surface_->underMouse() && content.contains(surface_->mapFromGlobal(QCursor::pos()));
    const bool focused = surface_->hasFocus() && surface_->window()->isActiveWindow();
    const bool hideLocal = cursor_.hidesLocalPointer(available_, focused, inside);
    if (hideLocal != localCursorHidden_) {
        localCursorHidden_ = hideLocal;
        if (hideLocal) surface_->setCursor(Qt::BlankCursor);
        else surface_->unsetCursor();
    }
    if (!cursor_.separate()) {
        surface_->setRemoteCursor({}, {});
        return;
    }

    surface_->setRemoteCursor(cursor_.image(), cursor_.rectangle(hideLocal));
}
bool ViewerInput::eventFilter(QObject* object, QEvent* event) {
    if (!available_)
        return false;
    const auto type = event->type();
    if ((type == QEvent::FocusOut && object == surface_) ||
        (type == QEvent::WindowDeactivate && object == surface_->window()) ||
        (type == QEvent::ApplicationStateChange &&
         QApplication::applicationState() != Qt::ApplicationActive)) {
        release();
        return false;
    }
    if (object != surface_)
        return false;
    if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::FocusIn) {
        if (type == QEvent::Leave) {
            optimisticCursor_.stop();
            cursor_.expirePrediction();
        }
        updateCursor();
    }
    if (type == QEvent::ShortcutOverride || type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        auto* e = static_cast<QKeyEvent*>(event);
        if (e->key() == Qt::Key_F11 && e->modifiers() == (Qt::ControlModifier | Qt::AltModifier)) {
            release();
            return false;
        }
    }
    if (type == QEvent::ShortcutOverride && surface_->hasFocus()) {
        auto* e = static_cast<QKeyEvent*>(event);
        if (!physicalCode(*e).isEmpty()) {
            event->accept();
            return true;
        }
    }
    if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        auto* e = static_cast<QKeyEvent*>(event);
        const QString code = physicalCode(*e);
        if (code.isEmpty())
            return false;
        const bool down = type == QEvent::KeyPress;
        if (!down && e->isAutoRepeat())
            return true;
        if (down || keys_.contains(code)) {
            if (send_({{"type", "key"}, {"code", code}, {"down", down}}) && down)
                keys_.insert(code);
            if (!down)
                keys_.remove(code);
        }
        return true;
    }
    const auto at = [&](QPointF position) {
        const QRectF r = surface_->contentRect();
        if (r.isEmpty() || !r.contains(position))
            return false;
        const QPointF normalized((position.x() - r.x()) / r.width(), (position.y() - r.y()) / r.height());
        QJsonObject message{{"type", "move"}, {"x", normalized.x()}, {"y", normalized.y()}};
        const bool predict = cursor_.separate();
        const auto serial = cursor_.nextMoveSerial();
        if (predict) message.insert("cursorMove", static_cast<qint64>(serial));
        const bool sent = send_(std::move(message));
        if (sent && predict) {
            cursor_.predict(serial, normalized);
            optimisticCursor_.start();
            updateCursor();
        }
        return sent;
    };
    if (type == QEvent::Wheel) {
        auto* e = static_cast<QWheelEvent*>(event);
        if (!at(e->position()))
            return false;
        const int delta = qBound(-32768, e->angleDelta().y(), 32767);
        if (delta)
            send_({{"type", "wheel"}, {"delta", delta}});
        return true;
    }
    if (type == QEvent::MouseMove || type == QEvent::MouseButtonPress ||
        type == QEvent::MouseButtonDblClick || type == QEvent::MouseButtonRelease) {
        auto* e = static_cast<QMouseEvent*>(event);
        if (type == QEvent::MouseMove) {
            if (surface_->hasFocus())
                at(e->position());
            updateCursor();
            return false;
        }
        const auto button = mouseButton(e->button());
        if (!button)
            return false;
        if (type == QEvent::MouseButtonRelease) {
            if (!buttons_.remove(*button))
                return false;
            at(e->position());
            send_({{"type", "button"}, {"button", *button}, {"down", false}});
        } else {
            if (!at(e->position()))
                return false;
            surface_->setFocus(Qt::MouseFocusReason);
            if (send_({{"type", "button"}, {"button", *button}, {"down", true}}))
                buttons_.insert(*button);
        }
        return true;
    }
    return false;
}
} // namespace evp
