#include "floating_surface.h"
#include "visual_style.h"

#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QPalette>
#include <QScreen>
#include <QTimer>
#include <QWidget>

namespace native_ui {
namespace {

class ThemedToolTip final : public QWidget {
public:
    ThemedToolTip(QWidget& anchor, const QString& text)
        : QWidget(&anchor, Qt::ToolTip | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus),
          anchor_(anchor) {
        setObjectName(QStringLiteral("themedToolTip"));
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* caption = label(text, "tipCaption", 14, QFont::Medium);
        layout->addWidget(caption);
        styleFloatingSurface(*this, *caption);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setThemedStyle(*caption, QStringLiteral(
            "QLabel#tipCaption { color: #263854/#e6e8ec@foreground; background: #ffffff/#303238@raised;"
            " border: 1px solid #ced8e6/#545860@stroke; border-radius: 9px; padding: 8px 13px; }"));
        anchor.setToolTip(text);
        anchor.installEventFilter(this);
        lifetime_.setSingleShot(true);
        connect(&lifetime_, &QTimer::timeout, this, &QWidget::hide);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == &anchor_) {
            switch (event->type()) {
            case QEvent::ToolTip: {
                adjustSize();
                const auto bounds = anchor_.screen()->availableGeometry();
                const auto origin = anchor_.mapToGlobal(QPoint(0, 0));
                const int x = origin.x() + (anchor_.width() - width()) / 2;
                int y = origin.y() + anchor_.height() - 4;
                if (y + height() > bounds.bottom() + 1)
                    y = origin.y() - height() + 4;
                move(qBound(bounds.left(), x, qMax(bounds.left(), bounds.right() + 1 - width())),
                     qMax(bounds.top(), y));
                show();
                lifetime_.start(6000);
                return true;
            }
            case QEvent::Hide:
            case QEvent::Leave:
            case QEvent::WindowDeactivate:
            case QEvent::MouseButtonPress:
            case QEvent::KeyPress:
                hide();
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter(object, event);
    }

private:
    QWidget& anchor_;
    QTimer lifetime_;
};

class SurfaceShadow final : public QGraphicsDropShadowEffect {
public:
    explicit SurfaceShadow(QWidget& surface) : QGraphicsDropShadowEffect(&surface) {
        setColor(QColor(24, 40, 68, 52));
        updateScale(surface.devicePixelRatioF());
        surface.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        constexpr auto scaleChange = QEvent::DevicePixelRatioChange;
#else
        constexpr auto scaleChange = QEvent::ScreenChangeInternal;
#endif
        if (event->type() == QEvent::Show || event->type() == scaleChange)
            updateScale(static_cast<QWidget*>(object)->devicePixelRatioF());
        return QGraphicsDropShadowEffect::eventFilter(object, event);
    }

private:
    void updateScale(qreal scale) {
        setBlurRadius(10 * scale);
        setOffset(0, 3 * scale);
    }
};

}

void clearWindowDecoration(QWidget& window) {
    window.ensurePolished();
    window.setGraphicsEffect(nullptr);
    if (auto* frame = qobject_cast<QFrame*>(&window))
        frame->setFrameStyle(QFrame::NoFrame);
}

void attachThemedToolTip(QWidget& anchor, const QString& text) {
    new ThemedToolTip(anchor, text);
}

void styleFloatingSurface(QWidget& window, QWidget& surface) {
    window.setWindowFlags(window.windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    window.setAttribute(Qt::WA_TranslucentBackground);
    window.setAutoFillBackground(false);
    auto palette = window.palette();
    palette.setColor(QPalette::Window, Qt::transparent);
    window.setPalette(palette);
    clearWindowDecoration(window);
    window.setContentsMargins(12, 9, 12, 15);
    surface.setGraphicsEffect(new SurfaceShadow(surface));
}

}
