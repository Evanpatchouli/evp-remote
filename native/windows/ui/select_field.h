#pragma once

#include <QComboBox>

class QLabel;

namespace native_ui {

class SelectField final : public QComboBox {
    Q_OBJECT
public:
    explicit SelectField(QWidget* parent = nullptr);
    void showPopup() override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

SelectField* codeLifetimeSelector(QLabel* label, const char* name);

}
