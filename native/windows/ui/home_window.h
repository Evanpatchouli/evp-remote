#pragma once

#include <QMainWindow>
#include <QUrl>

class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;

class HomeWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit HomeWindow(QWidget* parent = nullptr);

public slots:
    void setSignalingUrl(const QUrl& url);
    void setBusy(bool busy);
    void setSessionActive(bool active);
    void setStatus(const QString& message);
    void showFailure(const QString& message);

signals:
    void joinRequested(const QUrl& signalingUrl, const QString& invitation);
    void cancelRequested();
    void sessionActivationRequested();

private:
    void requestJoin();
    void pasteInvitation();
    void updateBanner();
    void updateForm();

    QFrame* banner_;
    QLabel* bannerIcon_;
    QLabel* bannerTitle_;
    QLabel* bannerMessage_;
    QLineEdit* invitation_;
    QLineEdit* signalingUrl_;
    QPushButton* pasteButton_;
    QPushButton* joinButton_;
    QPushButton* cancelButton_;
    QScrollArea* scroll_;
    QString statusMessage_;
    bool busy_ = false;
    bool sessionActive_ = false;
    bool failed_ = false;
};
