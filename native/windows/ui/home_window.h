#pragma once

#include <QMainWindow>
#include <QUrl>
#include <memory>

class DevicePage;
class QFrame;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;

class HomeWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit HomeWindow(QWidget* parent = nullptr);
    ~HomeWindow() override;
    DevicePage& devices();
    void showDevicePage();

public slots:
    void setSignalingUrl(const QUrl& url);
    void setBusy(bool busy, bool trusted = false);
    void setSessionActive(bool active);
    bool setHostBusy(bool busy, bool trusted = false);
    void setHostInvitation(const QString& invitation);
    void setHostPending(quint64 requestId);
    void setHostActive(bool active);
    void setStatus(const QString& message);
    void showFailure(const QString& message);

signals:
    void joinRequested(const QUrl& signalingUrl, const QString& invitation);
    void cancelRequested();
    void sessionActivationRequested();
    void hostRequested(const QUrl& signalingUrl, const QString& key);
    void hostApproved(quint64 requestId, bool highRefresh, bool relayOnly, bool withAudio);
    void hostDenied(quint64 requestId);
    void hostEndRequested();
    void hostDesktopUnavailable();

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    QWidget* createHostPage();
    void switchRole(bool host);
    QUrl requestedSignalingUrl(QLineEdit* field);
    void requestJoin();
    void requestHost();
    void pasteInvitation();
    void copyHostInvitation();
    void respondToHostRequest(bool approved);
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
    QPushButton* viewerNavigation_;
    QPushButton* hostNavigation_;
    QPushButton* deviceNavigation_;
    QStackedWidget* pages_;
    DevicePage* devicePage_;
    QScrollArea* scroll_;
    QWidget* hostSetup_;
    QWidget* hostInvite_;
    QWidget* hostConsent_;
    QLineEdit* hostKey_;
    QLineEdit* hostSignalingUrl_;
    QLineEdit* hostInvitation_;
    QPushButton* hostEndButton_;
    std::unique_ptr<QPushButton> trustedShareStop_;
    WId notificationWindow_ = 0;
    QCheckBox* highRefresh_;
    QCheckBox* shareAudio_;
    QCheckBox* relayOnly_;
    QString statusMessage_;
    quint64 hostRequestId_ = 0;
    bool busy_ = false;
    bool sessionActive_ = false;
    bool hostPage_ = false;
    bool devicePageSelected_ = false;
    bool trustedSession_ = false;
    bool hostBusy_ = false;
    bool hostActive_ = false;
    bool failed_ = false;
};
