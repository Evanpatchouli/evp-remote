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
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTemporaryDir;

namespace evp {

class ClientUpdater final : public QObject {
public:
    explicit ClientUpdater(HomeWindow& home);
    ~ClientUpdater() override;

private:
    enum class State { Idle, Checking, Rechecking, Downloading, Restarting };

    void check(bool manual);
    void startMetadata(State state);
    void readMetadata();
    void finishMetadata();
    void startDownload();
    void readPayload();
    void finishDownload();
    void applyUpdate();
    void setStatus(const QString& text);
    void abortTransfer(const QString& error);
    void fail(const QString& message);
    void closeDialog();

    QNetworkAccessManager network_;
    QTimer periodic_, deadline_;
    QNetworkReply* reply_ = nullptr;
    QDialog* dialog_;
    QLabel* status_;
    QPlainTextEdit* notes_;
    QProgressBar* progress_;
    QPushButton* install_;
    QPushButton* close_;
    State state_ = State::Idle;
    bool manual_ = false;
    QString lastOffered_, transferError_;
    QByteArray metadata_, prefix_;
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
