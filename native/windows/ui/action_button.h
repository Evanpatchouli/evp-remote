#pragma once

#include <QIcon>
#include <QPushButton>

namespace native_ui {

class ActionButton : public QPushButton {
public:
    explicit ActionButton(const QString& text, QWidget* parent = nullptr)
        : ActionButton(QIcon{}, text, parent) {}

    ActionButton(const QIcon& icon, const QString& text, QWidget* parent = nullptr)
        : QPushButton(icon, text, parent) {
        setMinimumHeight(36);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet(QStringLiteral("QPushButton { border-radius: 8px; outline: none; }"));
    }
};

} // namespace native_ui
