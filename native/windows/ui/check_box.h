#pragma once

#include <QCheckBox>
#include <QEvent>

namespace native_ui {

class CheckBox final : public QCheckBox {
public:
    explicit CheckBox(const QString& text, QWidget* parent = nullptr) : QCheckBox(text, parent) {
        updateCursor();
    }

protected:
    void changeEvent(QEvent* event) override {
        QCheckBox::changeEvent(event);
        if (event->type() == QEvent::EnabledChange) updateCursor();
    }

private:
    void updateCursor() { setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor); }
};

} // namespace native_ui
