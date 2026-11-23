#pragma once

#include "theme.h"

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QTransform>

namespace native_ui {

enum class Symbol {
    Connect,
    Display,
    Arrow,
    Back,
    ChevronDown,
    Check,
    Refresh,
    Download,
    Clipboard,
    Warning,
    End,
    Power,
    Settings,
    Rename, MoveUp, MoveDown, Pin, Unpin, Remove,
};

inline QPixmap symbolPixmap(Symbol symbol, const QColor& color, int size)
{
    constexpr int scale = 3;
    QPixmap pixmap(size * scale, size * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto unit = size / 24.0;
    painter.scale(unit, unit);
    painter.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    switch (symbol) {
    case Symbol::Rename:
        painter.drawPolygon(QPolygonF{{4, 20}, {5, 15}, {16, 4}, {20, 8}, {9, 19}});
        painter.drawLine(QPointF(14, 6), QPointF(18, 10));
        break;
    case Symbol::MoveDown:
    case Symbol::MoveUp:
        if (symbol == Symbol::MoveDown) {
            painter.translate(24, 24);
            painter.rotate(180);
        }
        painter.drawLine(QPointF(12, 20), QPointF(12, 4));
        painter.drawPolyline(QPolygonF{{5, 11}, {12, 4}, {19, 11}});
        break;
    case Symbol::Pin:
    case Symbol::Unpin:
        if (symbol == Symbol::Unpin)
            painter.drawLine(QPointF(3, 3), QPointF(21, 21));
        painter.translate(12, 12);
        painter.rotate(32);
        painter.drawPolygon(QPolygonF{{-4,-8}, {4,-8}, {3,-2}, {6,2}, {6,3}, {-6,3}, {-6,2}, {-3,-2}});
        painter.drawLine(QPointF(0, 3), QPointF(0, 10));
        break;
    case Symbol::Remove:
        painter.drawLine(QPointF(4, 7), QPointF(20, 7));
        painter.drawLine(QPointF(9, 4), QPointF(15, 4));
        painter.drawPolyline(QPolygonF{{6, 9}, {7, 20}, {17, 20}, {18, 9}});
        painter.drawLine(QPointF(10, 10), QPointF(10.5, 17));
        painter.drawLine(QPointF(14, 10), QPointF(13.5, 17));
        break;
    case Symbol::Connect:
        painter.drawRoundedRect(QRectF(2.7, 4.3, 11.8, 10.2), 2, 2);
        painter.drawLine(QPointF(8.6, 14.5), QPointF(8.6, 18.6));
        painter.drawLine(QPointF(5.4, 18.6), QPointF(11.7, 18.6));
        painter.drawLine(QPointF(13.6, 15.5), QPointF(21.2, 15.5));
        painter.drawLine(QPointF(18.2, 12.5), QPointF(21.2, 15.5));
        painter.drawLine(QPointF(18.2, 18.5), QPointF(21.2, 15.5));
        break;
    case Symbol::Display:
        painter.drawRoundedRect(QRectF(3, 4.3, 18, 12.5), 2, 2);
        painter.drawLine(QPointF(12, 16.8), QPointF(12, 20.1));
        painter.drawLine(QPointF(8.5, 20.1), QPointF(15.5, 20.1));
        break;
    case Symbol::Back:
        painter.translate(24, 0);
        painter.scale(-1, 1);
        [[fallthrough]];
    case Symbol::Arrow:
        painter.drawLine(QPointF(4.5, 12), QPointF(19, 12));
        painter.drawLine(QPointF(13.7, 6.7), QPointF(19, 12));
        painter.drawLine(QPointF(13.7, 17.3), QPointF(19, 12));
        break;
    case Symbol::ChevronDown:
        painter.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(QPolygonF{QPointF(5, 9), QPointF(12, 16), QPointF(19, 9)});
        break;
    case Symbol::Check:
        painter.drawEllipse(QRectF(3.2, 3.2, 17.6, 17.6));
        painter.drawPolyline(QPolygonF{QPointF(7.6, 12.2), QPointF(10.6, 15.2), QPointF(16.8, 8.8)});
        break;
    case Symbol::Refresh:
        painter.drawArc(QRectF(4, 4, 16, 16), 30 * 16, 300 * 16);
        painter.drawPolyline(QPolygonF{QPointF(20, 4), QPointF(20, 9), QPointF(15, 9)});
        break;
    case Symbol::Download:
        painter.drawLine(QPointF(12, 3.5), QPointF(12, 14.5));
        painter.drawPolyline(QPolygonF{QPointF(7.5, 10), QPointF(12, 14.5), QPointF(16.5, 10)});
        painter.drawPolyline(QPolygonF{QPointF(4.5, 15.5), QPointF(4.5, 19.5), QPointF(19.5, 19.5), QPointF(19.5, 15.5)});
        break;
    case Symbol::Clipboard:
        painter.drawRoundedRect(QRectF(5.8, 5.5, 12.4, 15), 2, 2);
        painter.setBrush(themeColor("#f8faff", "#222427", "surface"));
        painter.drawRoundedRect(QRectF(8.4, 3.1, 7.2, 4.8), 1.2, 1.2);
        painter.drawLine(QPointF(9, 12), QPointF(15, 12));
        painter.drawLine(QPointF(9, 15.5), QPointF(15, 15.5));
        break;
    case Symbol::Warning:
        painter.drawEllipse(QRectF(3.2, 3.2, 17.6, 17.6));
        painter.drawLine(QPointF(12, 7.4), QPointF(12, 13));
        painter.drawPoint(QPointF(12, 16.5));
        break;
    case Symbol::End:
        painter.drawRoundedRect(QRectF(5.5, 5.5, 13, 13), 2.4, 2.4);
        break;
    case Symbol::Power:
        painter.drawArc(QRectF(4, 4.5, 16, 16), 120 * 16, 300 * 16);
        painter.drawLine(QPointF(12, 2.5), QPointF(12, 11.5));
        break;
    case Symbol::Settings: {
        QPolygonF outline;
        QTransform rotation;
        rotation.translate(12, 12);
        for (int tooth = 0; tooth < 8; ++tooth) {
            for (const QPointF point : {QPointF(-2.6, -7.1), QPointF(-1.6, -9.4),
                                        QPointF(1.6, -9.4), QPointF(2.6, -7.1)})
                outline << rotation.map(point);
            rotation.rotate(45);
        }
        painter.drawPolygon(outline);
        painter.drawEllipse(QPointF(12, 12), 3.1, 3.1);
        break;
    }
    }
    return pixmap;
}

inline QIcon symbolIcon(Symbol symbol, const QColor& color, int size)
{
    return QIcon(symbolPixmap(symbol, color, size));
}

inline QIcon brandIcon()
{
    return QIcon(QStringLiteral(":/evp/evp-remote-logo.png"));
}

inline QFont uiFont(int pixels, QFont::Weight weight = QFont::Normal)
{
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

inline QLabel* label(const QString& text, const char* name, int pixels,
                     QFont::Weight weight = QFont::Normal)
{
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setObjectName(QString::fromLatin1(name));
    result->setFont(uiFont(pixels, weight));
    return result;
}

} // namespace native_ui
