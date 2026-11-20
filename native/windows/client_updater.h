#pragma once

#include "update_release.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QTimer>

#include <memory>

class HomeWindow;
class QLabel;
class QDialog;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QTemporaryDir;
class QTextBrowser;
class QWidget;

namespace evp {

class ClientUpdater final : public QObject {
public:
    explicit ClientUpdater(HomeWindow& home, const QUrl& releaseEndpoint = {});
    ~ClientUpdater() override;

private:
    enum class State { Idle, Checking, Rechecking, Downloading, Restarting };
    enum class Appearance { Activity, Current, Offer, Warning };

    void check(bool manual);
    void startMetadata(State state);
    void readMetadata();
    void finishMetadata();
    void startDownload();
    void readPayload();
    void finishDownload();
    void applyUpdate();
    bool canInstall() const;
    void setStatus(Appearance appearance, const QString& heading, const QString& detail = {});
    void abortTransfer(const QString& error);
    void fail(const QString& message);
    void closeDialog();

    QNetworkAccessManager network_;
    QTimer periodic_, deadline_;
    QNetworkReply* reply_ = nullptr;
    QDialog* dialog_;
    QLabel* mark_;
    QLabel* status_;
    QLabel* detail_;
    QWidget* versions_;
    QLabel* currentVersion_;
    QLabel* latestVersion_;
    QTextBrowser* notes_;
    QProgressBar* progress_;
    QPushButton* install_;
    QPushButton* close_;
    State state_ = State::Idle;
    bool manual_ = false;
    QString lastOffered_, transferError_;
    QByteArray metadata_, prefix_;
    const QUrl releaseEndpoint_;
    const QString launcherPath_;
    const QString launcherPid_;
    const bool portable_;
    std::optional<UpdateRelease> release_;
    std::unique_ptr<QTemporaryDir> staging_;
    QFile download_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    qint64 received_ = 0;
};

}
