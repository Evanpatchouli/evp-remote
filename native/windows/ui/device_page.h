#pragma once

#include "devices/device_identity.h"
#include <QSet>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class DevicePage final : public QWidget {
    Q_OBJECT

public:
    explicit DevicePage(QWidget* parent = nullptr);
    void setIdentity(const QString& id);
    void setServer(const QUrl& url);
    void setRegistration(bool online, const QString& status);
    void setActivity(bool sessionBusy, bool pending);
    void setPreferences(const evp::DevicePreferences& preferences, bool editable);
    void setDevices(const QMap<QString, evp::DevicePeer>& peers, const QSet<QString>& online);
    void setPairingCode(const QString& code);
    void setPairingRequest(const QString& attempt, const QString& name, const QString& id);
    void setMessage(const QString& text);
    void setReturnVisible(bool visible);

signals:
    void configureRequested(const QUrl& url, const QString& key);
    void codeRequested();
    void pairingRequested(const QString& code);
    void pairingDecided(const QString& attempt, bool confirmed);
    void connectRequested(const QString& id);
    void removeRequested(const QString& id);
    void preferencesChanged(bool allowIncoming, bool audio, bool highRefresh, bool relayOnly);
    void cancelRequested();
    void returnRequested();

private:
    QString selectedId() const;
    void updateActions();

    QLabel* registration_;
    QLabel* message_;
    QLineEdit* id_;
    QLineEdit* code_;
    QLineEdit* peerCode_;
    QLineEdit* url_;
    QLineEdit* key_;
    QListWidget* list_;
    QWidget* pairApproval_;
    QLabel* pairRequester_;
    QPushButton* copyId_;
    QPushButton* makeCode_;
    QPushButton* copyCode_;
    QPushButton* pair_;
    QPushButton* connect_;
    QPushButton* remove_;
    QPushButton* configure_;
    QPushButton* cancel_;
    QPushButton* return_;
    QCheckBox* incoming_;
    QCheckBox* audio_;
    QCheckBox* highRefresh_;
    QCheckBox* relayOnly_;
    QSet<QString> online_;
    QString pendingApproval_;
    bool registered_ = false;
    bool preferencesEditable_ = false;
    bool sessionBusy_ = false;
    bool pending_ = false;
};
