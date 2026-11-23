#pragma once

#include "code_options.h"
#include "devices/device_identity.h"
#include <QSet>
#include <QWidget>
#include <optional>

class QAction;
class QCheckBox;
class QComboBox;
class QInputDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;

class DevicePage final : public QWidget {
    Q_OBJECT

public:
    explicit DevicePage(QWidget* parent = nullptr);
    void setIdentity(const QString& id);
    void setConnectionConfigured(bool configured);
    void setRegistration(bool online, const QString& status);
    void setActivity(bool sessionBusy, bool pending);
    void setPreferences(const evp::DevicePreferences& preferences, bool editable);
    void setDevices(const QMap<QString, evp::DevicePeer>& peers, const QSet<QString>& online);
    void setPairingCode(const QString& code, evp::CodeOptions options = {});
    void setPairingRequest(const QString& attempt, const QString& name, const QString& id);
    void setMessage(const QString& text);
    void clearMessageIf(const QString& text);
    void setReturnVisible(bool visible);

signals:
    void codeRequested(evp::CodeOptions options);
    void pairingOptionsEdited();
    void pairingRequested(const QString& code);
    void pairingDecided(const QString& attempt, bool confirmed);
    void connectRequested(const QString& id);
    void removeRequested(const QString& id);
    void renameRequested(const QString& id, const QString& name);
    void moveRequested(const QString& id, evp::DeviceMove move);
    void pinRequested(const QString& id, bool pinned);
    void preferencesChanged(bool allowIncoming, bool audio, bool relayOnly);
    void cancelRequested();
    void returnRequested();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    QString selectedId() const;
    QListWidgetItem* deviceItem(const QString& id) const;
    QListWidgetItem* pinItemAt(const QPoint& position) const;
    bool handlePinEvent(QEvent* event);
    void setPinHoverPosition(std::optional<QPoint> position);
    void refreshPinHover();
    void toggleDevicePin(const QString& id);
    void showCurrentDeviceMenu();
    void showDeviceMenu(QListWidgetItem* item, const QPoint& globalPosition);
    void updateDeviceMenu();
    QString takeContextDeviceId();
    bool canMoveDevice(const QListWidgetItem* item, evp::DeviceMove move) const;
    void renameContextDevice();
    void moveContextDevice(evp::DeviceMove move);
    void updateActions();

    QLabel* registration_;
    QLabel* message_;
    QLineEdit* id_;
    QLineEdit* code_;
    QLineEdit* peerCode_;
    QLabel* settingsHint_;
    QListWidget* list_;
    QLabel* emptyList_;
    QMenu* deviceMenu_;
    QAction* moveUp_;
    QAction* moveDown_;
    QAction* pin_;
    QInputDialog* renameDialog_;
    QComboBox* pairingLifetime_;
    QCheckBox* autoAcceptPairing_;
    QLabel* issuedCodeDetails_;
    QWidget* pairApproval_;
    QLabel* pairRequester_;
    QPushButton* copyId_;
    QPushButton* makeCode_;
    QPushButton* copyCode_;
    QPushButton* pair_;
    QPushButton* connect_;
    QPushButton* remove_;
    QPushButton* cancel_;
    QPushButton* return_;
    QCheckBox* incoming_;
    QCheckBox* audio_;
    QCheckBox* relayOnly_;
    QSet<QString> online_;
    QString contextId_;
    QString renameId_;
    QString pressedPinId_;
    QString pendingApproval_;
    std::optional<QPoint> pinHoverPosition_;
    bool pinMouseGesture_ = false;
    bool previousPressWasPin_ = false;
    bool registered_ = false;
    bool preferencesEditable_ = false;
    bool sessionBusy_ = false;
    bool pending_ = false;
};
