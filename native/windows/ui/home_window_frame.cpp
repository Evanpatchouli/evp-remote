#include "home_window_frame.h"

#include "theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>
#include <QtMath>

#include <Windows.h>
#include <windowsx.h>

namespace native_ui {
namespace {

enum class CaptionGlyph { Minimize, Maximize, Restore, Close };

bool calculateClientArea(const MSG& message, qintptr& result)
{
    RECT& client = message.wParam
        ? reinterpret_cast<NCCALCSIZE_PARAMS*>(message.lParam)->rgrc[0]
        : *reinterpret_cast<RECT*>(message.lParam);
    const RECT frame = client;
    result = DefWindowProcW(message.hwnd, message.message, message.wParam, message.lParam);
    if (IsZoomed(message.hwnd)) {
        // The maximized resize frame extends past the work area. Keep it off-screen.
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        client.top = frame.top + (client.left - frame.left);
        if (GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &monitor))
            client.top = qMax(client.top, monitor.rcWork.top);
    } else {
        client.top = frame.top;
    }
    return true;
}

POINT screenPoint(LPARAM coordinates)
{
    return {GET_X_LPARAM(coordinates), GET_Y_LPARAM(coordinates)};
}

} // namespace

class HomeCaptionButton final : public QToolButton {
public:
    HomeCaptionButton(QWidget* parent, const char* name, CaptionGlyph glyph, const QString& label)
        : QToolButton(parent), glyph_(glyph)
    {
        setObjectName(QString::fromLatin1(name));
        setFixedSize(46, 32);
        setFocusPolicy(Qt::TabFocus);
        setAccessibleName(label);
        setToolTip(label);
        onThemeChanged(*this, [this] { update(); });
    }

    void setMaximized(bool maximized)
    {
        glyph_ = maximized ? CaptionGlyph::Restore : CaptionGlyph::Maximize;
        setAccessibleName(maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
        setToolTip(accessibleName());
        update();
    }

    bool nativeHovered() const { return nativeHovered_; }

    void setNativeHovered(bool hovered)
    {
        if (nativeHovered_ == hovered)
            return;
        nativeHovered_ = hovered;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool highlighted = isDown() || underMouse() || nativeHovered_;
        const bool close = glyph_ == CaptionGlyph::Close;
        if (highlighted) {
            const auto background = close
                ? QColor(isDown() ? "#b91c2c" : "#c42b39")
                : isDown() ? themeColor("#dce7f8", "#3a404a", "accentHoverTint")
                           : themeColor("#e8effa", "#2d323a", "raised");
            painter.fillRect(rect(), background);
        }
        if (hasFocus()) {
            painter.setPen(themeColor("#3475e4", "#739ff1", "accentMark"));
            painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 3, 3);
        }

        const auto ink = highlighted && close ? QColor(Qt::white)
            : themeColor("#182844", "#e6e8ec", "foreground");
        painter.setPen(QPen(ink, 1, Qt::SolidLine, Qt::FlatCap));
        painter.translate(width() / 2.0, height() / 2.0);
        switch (glyph_) {
        case CaptionGlyph::Minimize:
            painter.drawLine(QPointF(-5.5, 0.5), QPointF(5.5, 0.5));
            break;
        case CaptionGlyph::Maximize:
            painter.drawRect(QRectF(-4.5, -4.5, 9, 9));
            break;
        case CaptionGlyph::Restore:
            painter.drawPolyline(QPolygonF{QPointF(-2.5, -2.5), QPointF(-2.5, -5.5),
                                           QPointF(5.5, -5.5), QPointF(5.5, 2.5),
                                           QPointF(2.5, 2.5)});
            painter.drawRect(QRectF(-5.5, -2.5, 8, 8));
            break;
        case CaptionGlyph::Close:
            painter.drawLine(QPointF(-4.5, -4.5), QPointF(4.5, 4.5));
            painter.drawLine(QPointF(-4.5, 4.5), QPointF(4.5, -4.5));
            break;
        }
    }

private:
    CaptionGlyph glyph_;
    bool nativeHovered_ = false;
};

HomeWindowFrame::HomeWindowFrame(QWidget& home, QLayout& brand, QLayout& settingsHeader)
    : QWidget(&home), home_(home), brand_(brand), settingsHeader_(settingsHeader)
{
    setObjectName(QStringLiteral("homeCaption"));
    setFixedHeight(32);
    setLayoutDirection(Qt::LeftToRight);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch();
    auto* minimize = new HomeCaptionButton(this, "homeMinimize", CaptionGlyph::Minimize,
                                          QStringLiteral("最小化"));
    maximize_ = new HomeCaptionButton(this, "homeMaximize", CaptionGlyph::Maximize,
                                      QStringLiteral("最大化"));
    auto* close = new HomeCaptionButton(this, "homeClose", CaptionGlyph::Close,
                                       QStringLiteral("关闭"));
    layout->addWidget(minimize);
    layout->addWidget(maximize_);
    layout->addWidget(close);

    connect(minimize, &QToolButton::clicked, &home_, &QWidget::showMinimized);
    connect(maximize_, &QToolButton::clicked, &home_, [this] {
        if (home_.isMaximized())
            home_.showNormal();
        else
            home_.showMaximized();
    });
    connect(close, &QToolButton::clicked, &home_, &QWidget::close);
    home_.installEventFilter(this);
}

void HomeWindowFrame::prepareWindow()
{
    const WId id = home_.winId();
    if (id == preparedWindow_ || !IsWindow(reinterpret_cast<HWND>(id)))
        return;
    preparedWindow_ = id;
    // Qt creates the HWND before it can receive the widget's native events.
    SetWindowPos(reinterpret_cast<HWND>(id), nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool HomeWindowFrame::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == &home_ && event->type() == QEvent::WindowStateChange)
        maximize_->setMaximized(home_.isMaximized());
    return QWidget::eventFilter(watched, event);
}

bool HomeWindowFrame::maximizeButtonContains(const QPointF& position) const
{
    const QPoint point(qFloor(position.x()), qFloor(position.y()));
    return QRect(maximize_->mapTo(&home_, QPoint()), maximize_->size()).contains(point);
}

qintptr HomeWindowFrame::hitTest(const MSG& message) const
{
    POINT point = screenPoint(message.lParam);
    RECT frame{};
    GetWindowRect(message.hwnd, &frame);
    if (!IsZoomed(message.hwnd) && PtInRect(&frame, point)) {
        const UINT dpi = GetDpiForWindow(message.hwnd);
        const int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int top = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + padding;
        if (point.y < frame.top + top) {
            const int side = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + padding;
            if (point.x < frame.left + side)
                return HTTOPLEFT;
            if (point.x >= frame.right - side)
                return HTTOPRIGHT;
            return HTTOP;
        }
    }

    ScreenToClient(message.hwnd, &point);
    RECT client{};
    GetClientRect(message.hwnd, &client);
    if (!PtInRect(&client, point))
        return DefWindowProcW(message.hwnd, message.message, message.wParam, message.lParam);

    const QPointF position = QPointF(point.x, point.y) / home_.devicePixelRatioF();
    const QPoint local(qFloor(position.x()), qFloor(position.y()));
    const QPoint captionPoint = mapFrom(&home_, local);
    if (rect().contains(captionPoint)) {
        if (maximizeButtonContains(position))
            return HTMAXBUTTON;
        return childAt(captionPoint) ? HTCLIENT : HTCAPTION;
    }
    for (auto* header : {&brand_, &settingsHeader_}) {
        auto* sidebar = header->parentWidget();
        if (!sidebar->isVisible())
            continue;
        const QPoint sidebarPoint = sidebar->mapFrom(&home_, local);
        const QRect titleRegion(0, 0, sidebar->width(), header->geometry().bottom() + 1);
        if (titleRegion.contains(sidebarPoint) &&
            !qobject_cast<QAbstractButton*>(sidebar->childAt(sidebarPoint)))
            return HTCAPTION;
    }
    return HTCLIENT;
}

bool HomeWindowFrame::handleMessage(const MSG& message, qintptr& result)
{
    switch (message.message) {
    case WM_NCCALCSIZE:
        return calculateClientArea(message, result);
    case WM_NCHITTEST:
        result = hitTest(message);
        return true;
    case WM_NCMOUSEMOVE: {
        const bool hovering = message.wParam == HTMAXBUTTON;
        if (hovering && !maximize_->nativeHovered()) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_NONCLIENT | TME_LEAVE, message.hwnd, 0};
            TrackMouseEvent(&tracking);
        }
        maximize_->setNativeHovered(hovering);
        break;
    }
    case WM_NCMOUSELEAVE:
        maximize_->setNativeHovered(false);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (message.wParam == HTMAXBUTTON) {
            SetCapture(message.hwnd);
            maximizePressed_ = GetCapture() == message.hwnd;
            maximize_->setDown(maximizePressed_);
            result = 0;
            return true;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
        if (maximizePressed_) {
            // A non-client press becomes a client-coordinate drag while captured.
            const auto point = screenPoint(message.lParam);
            const bool inside = maximizeButtonContains(QPointF(point.x, point.y) / home_.devicePixelRatioF());
            if (message.message == WM_MOUSEMOVE) {
                maximize_->setDown(inside);
            } else {
                maximizePressed_ = false;
                maximize_->setDown(false);
                maximize_->setNativeHovered(false);
                ReleaseCapture();
                if (inside)
                    maximize_->click();
            }
            result = 0;
            return true;
        }
        break;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        if (maximizePressed_) {
            maximizePressed_ = false;
            maximize_->setDown(false);
            maximize_->setNativeHovered(false);
        }
        break;
    }
    return false;
}

} // namespace native_ui
