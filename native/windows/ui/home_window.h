#pragma once

#include "../code_options.h"

#include <QMainWindow>
#include <QUrl>
#include <memory>

class DevicePage;
class QFrame;
class QHideEvent;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QStackedWidget;
class QTabBar;

namespace native_ui {
class HomeWindowFrame;
}

class HomeWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit HomeWindow(QWidget* parent = nullptr);
    ~HomeWindow() override;
    DevicePage& devices();
    void showDevicePage();
    void alertConfirmation(const QString& message);
    void showPendingConfirmation();

public slots:
    void showSettingsPage();
    void setConnectionSettings(const QUrl& url, bool hasKey);
    void setConnectionSettingsEnabled(bool editable);
    void setSettingsMessage(const QString& message);
    void setBusy(bool busy, bool trusted = false);
    void setSessionActive(bool active);
    bool setHostBusy(bool busy, bool trusted = false);
    void setHostInvitation(const QString& invitation, int minutes = 5, bool autoApprove = false);
    void setHostPending(quint64 requestId);
    void setHostActive(bool active);
    void setStatus(const QString& message);
    void showFailure(const QString& message);

signals:
    void settingsRequested(const QUrl& url, const QString& key, bool forgetKey);
    void checkForUpdatesRequested();
    void joinRequested(const QString& invite);
    void cancelRequested();
    void sessionActivationRequested();
    void hostRequested(evp::CodeOptions options, bool highRefresh, bool relayOnly, bool withAudio);
    void hostApproved(quint64 requestId, bool highRefresh, bool relayOnly, bool withAudio);
    void hostDenied(quint64 requestId);
    void hostEndRequested();
    void hostDesktopUnavailable();
    void backgroundConfirmation(const QString& message);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    enum class Page { Assistance, Devices, Settings };
    enum class SettingsView { Connection, Appearance, About };
    QWidget* createHostPage();
    QWidget* createSettingsPage();
    void selectPage(Page page);
    void selectSettingsView(SettingsView view);
    void resetSettingsDraft();
    void updateKeyPlaceholder();
    void updateSettingsAvailability();
    void saveSettings();
    void switchRole(bool host, bool focusTab = false);
    void requestJoin();
    void requestHost();
    void pasteInvitation();
    void copyHostInvitation();
    void respondToHostRequest(bool approved);
    void updateBanner();
    void updateForm();
    void updateShareIndicator();

    QFrame* banner_;
    native_ui::HomeWindowFrame* frame_ = nullptr;
    QLabel* bannerIcon_;
    QLabel* bannerTitle_;
    QLabel* bannerMessage_;
    QLineEdit* invitation_;
    QPushButton* pasteButton_;
    QPushButton* joinButton_;
    QPushButton* cancelButton_;
    QPushButton* assistanceNavigation_;
    QWidget* assistanceHeader_;
    QTabBar* assistanceTabs_;
    QWidget* deviceTitle_;
    QWidget* deviceSummary_;
    QPushButton* deviceNavigation_;
    QPushButton* settingsNavigation_;
    QStackedWidget* rails_;
    QPushButton* settingsBack_;
    QPushButton* settingsConnectionNavigation_;
    QPushButton* settingsAppearanceNavigation_;
    QPushButton* settingsAboutNavigation_;
    QWidget* settingsPage_;
    QStackedWidget* settingsViews_;
    QWidget* settingsForm_;
    QLineEdit* settingsServerUrl_;
    QLineEdit* settingsDeploymentKey_;
    QCheckBox* forgetDeploymentKey_;
    QLabel* settingsMessage_;
    QLabel* settingsBusyMessage_;
    QWidget* viewerFormPage_;
    QWidget* hostFormPage_;
    DevicePage* devicePage_;
    QScrollArea* scroll_;
    QWidget* hostSetup_;
    QWidget* hostInvite_;
    QWidget* hostConsent_;
    QWidget* hostForm_;
    QWidget* shareOptions_;
    QWidget* hostDecisions_;
    QPushButton* hostCreate_;
    QPushButton* hostDeny_;
    QLineEdit* hostInvitation_;
    QLabel* invitationDetails_;
    QComboBox* invitationLifetime_;
    QCheckBox* autoAcceptInvitation_;
    QPushButton* hostEndButton_;
    std::unique_ptr<QWidget> shareIndicator_;
    WId notificationWindow_ = 0;
    QCheckBox* highRefresh_;
    QCheckBox* shareAudio_;
    QCheckBox* relayOnly_;
    QString statusMessage_;
    quint64 hostRequestId_ = 0;
    bool busy_ = false;
    bool sessionActive_ = false;
    bool hostPage_ = false;
    Page page_ = Page::Assistance;
    Page pageBeforeSettings_ = Page::Assistance;
    QUrl savedServerUrl_;
    bool hasSavedKey_ = false;
    bool settingsEditable_ = true;
    bool trustedSession_ = false;
    bool hostBusy_ = false;
    bool hostActive_ = false;
    bool issuedAutoApprove_ = false;
    bool failed_ = false;
};
