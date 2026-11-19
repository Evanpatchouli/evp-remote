#include "focus_style.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFocusEvent>
#include <QProxyStyle>
#include <QStyleOption>

namespace native_ui {
namespace {

class FocusStyle final : public QProxyStyle {
public:
    explicit FocusStyle(QApplication& app) : QProxyStyle(app.style()->name()) {
        app.installEventFilter(this);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override {
        if (element == PE_FrameFocusRect) {
            // Item views set State_KeyboardFocusChange even for mouse selection.
            if (!keyboardFocusVisible_)
                return;
            if (const auto* focus = qstyleoption_cast<const QStyleOptionFocusRect*>(option)) {
                auto keyboardFocus = *focus;
                keyboardFocus.state |= State_KeyboardFocusChange;
                QProxyStyle::drawPrimitive(element, &keyboardFocus, painter, widget);
                return;
            }
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::TouchBegin:
        case QEvent::TabletPress:
            setKeyboardFocusVisible(false);
            break;
        case QEvent::KeyPress:
        case QEvent::Shortcut:
            setKeyboardFocusVisible(true);
            break;
        case QEvent::FocusIn:
            switch (static_cast<QFocusEvent*>(event)->reason()) {
            case Qt::MouseFocusReason:
                setKeyboardFocusVisible(false);
                break;
            case Qt::TabFocusReason:
            case Qt::BacktabFocusReason:
            case Qt::ShortcutFocusReason:
                setKeyboardFocusVisible(true);
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
        return false;
    }

private:
    void setKeyboardFocusVisible(bool visible) {
        if (keyboardFocusVisible_ == visible)
            return;
        keyboardFocusVisible_ = visible;
        if (auto* focus = QApplication::focusWidget()) {
            focus->update();
            if (auto* view = qobject_cast<QAbstractItemView*>(focus))
                view->viewport()->update();
        }
    }

    bool keyboardFocusVisible_ = false;
};

}

void installFocusStyle(QApplication& app) {
    app.setStyle(new FocusStyle(app));
}

}
