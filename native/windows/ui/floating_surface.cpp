#include "floating_surface.h"

#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QPalette>
#include <QWidget>

namespace native_ui {
namespace {

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
