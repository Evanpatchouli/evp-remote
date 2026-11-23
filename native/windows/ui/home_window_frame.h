#pragma once

#include <QWidget>

struct tagMSG;

namespace native_ui {

class HomeCaptionButton;

class HomeWindowFrame final : public QWidget {
public:
    HomeWindowFrame(QWidget& home, QLayout& brand, QLayout& settingsHeader);
    void prepareWindow();
    bool handleMessage(const tagMSG& message, qintptr& result);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    qintptr hitTest(const tagMSG& message) const;
    bool maximizeButtonContains(const QPointF& position) const;

    QWidget& home_;
    QLayout& brand_;
    QLayout& settingsHeader_;
    HomeCaptionButton* maximize_;
    WId preparedWindow_ = 0;
    bool maximizePressed_ = false;
};

} // namespace native_ui
