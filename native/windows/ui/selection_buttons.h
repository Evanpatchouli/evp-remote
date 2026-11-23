#pragma once

#include <QCheckBox>
#include <QEvent>
#include <QRadioButton>

namespace native_ui {

template<class Button>
class SelectionButton final : public Button {
public:
    explicit SelectionButton(const QString& text, QWidget* parent = nullptr) : Button(text, parent) {
        updateCursor();
    }

protected:
    void changeEvent(QEvent* event) override {
        Button::changeEvent(event);
        if (event->type() == QEvent::EnabledChange) updateCursor();
    }

private:
    void updateCursor() { this->setCursor(this->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor); }
};

using CheckBox = SelectionButton<QCheckBox>;
using RadioButton = SelectionButton<QRadioButton>;

} // namespace native_ui
