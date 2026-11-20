#include "client_updater.h"

#include "ui/action_button.h"
#include "ui/home_window.h"
#include "ui/visual_style.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QVBoxLayout>

#include <utility>

namespace evp {
namespace {

constexpr qsizetype maximumMetadataSize = 256 * 1024;
constexpr int checkIntervalMs = 15 * 60 * 1000;
constexpr int downloadIdleTimeoutMs = 60 * 1000;
const auto releasesApi = QUrl("https://api.github.com/repos/Evanpatchouli/evp-remote/releases/latest");
const auto helperName = QStringLiteral("EvpRemoteUpdate.exe");

int statusCode(const QNetworkReply& reply) {
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

QNetworkRequest requestFor(const QUrl& url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "EVP-Remote/" + QCoreApplication::applicationVersion().toUtf8());
    return request;
}

}

ClientUpdater::ClientUpdater(HomeWindow& home)
    : QObject(&home),
      dialog_(new QDialog(&home)),
      launcherPath_(qEnvironmentVariable("EVP_PORTABLE_LAUNCHER_PATH")),
      launcherPid_(qEnvironmentVariable("EVP_PORTABLE_LAUNCHER_PID")),
      portable_(!launcherPath_.isEmpty() || !launcherPid_.isEmpty()) {
    dialog_->setObjectName("updateDialog");
    dialog_->setWindowTitle(QStringLiteral("检查更新 · EVP 远程"));
    dialog_->setMinimumWidth(440);
    dialog_->resize(480, 250);
    dialog_->setStyleSheet(QStringLiteral(
        "QDialog { background: #f7f9fd; }"
        "QLabel { color: #182844; }"
        "QPlainTextEdit { background: #ffffff; color: #263955; border: 1px solid #d9e3f1; border-radius: 8px; }"
        "QPushButton { background: #edf4ff; color: #214c98; border: 1px solid #b2c8eb; padding: 0 12px; }"
        "QPushButton:hover { background: #e0edff; }"
        "QPushButton:focus { border-color: #3475e4; }"
        "QPushButton#installUpdate { background: #2865d5; border-color: #2865d5; color: #ffffff; }"
        "QPushButton#installUpdate:hover { background: #2058c2; }"
        "QPushButton:disabled, QPushButton#installUpdate:disabled {"
        " background: #f4f7fc; color: #667893; border-color: #d9e3f1; }"));
    auto* layout = new QVBoxLayout(dialog_);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    status_ = native_ui::label({}, "updateStatus", 14, QFont::DemiBold);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    notes_ = new QPlainTextEdit;
    notes_->setReadOnly(true);
    notes_->setAccessibleName(QStringLiteral("更新说明"));
    notes_->setFont(native_ui::uiFont(13));
    layout->addWidget(notes_, 1);
    progress_ = new QProgressBar;
    progress_->setAccessibleName(QStringLiteral("更新下载进度"));
    layout->addWidget(progress_);
    auto* actions = new QHBoxLayout;
    actions->addStretch();
    close_ = new native_ui::ActionButton(QStringLiteral("关闭"));
    close_->setAutoDefault(false);
    actions->addWidget(close_);
    install_ = new native_ui::ActionButton(QStringLiteral("下载并更新"));
    install_->setObjectName("installUpdate");
    install_->setAutoDefault(false);
    actions->addWidget(install_);
    layout->addLayout(actions);

    connect(&home, &HomeWindow::checkForUpdatesRequested, this, [this] { check(true); });
    connect(close_, &QPushButton::clicked, dialog_, &QDialog::reject);
    connect(dialog_, &QDialog::finished, this, [this] { closeDialog(); });
    connect(install_, &QPushButton::clicked, this, [this] { startMetadata(State::Rechecking); });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        abortTransfer(QStringLiteral("更新请求超时，请重试。"));
    });
    deadline_.setSingleShot(true);
    periodic_.setTimerType(Qt::PreciseTimer);
    connect(&periodic_, &QTimer::timeout, this, [this] { check(false); });
    QTimer::singleShot(2000, this, [this] {
        check(false);
        periodic_.start(checkIntervalMs);
    });
    setStatus({});
}

ClientUpdater::~ClientUpdater() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
    }
    download_.close();
    delete dialog_;
}

void ClientUpdater::check(bool manual) {
    if (state_ == State::Restarting)
        return;
    if (manual) {
        manual_ = true;
        dialog_->setAttribute(Qt::WA_ShowWithoutActivating, false);
        dialog_->show();
        dialog_->raise();
        dialog_->activateWindow();
    }
    if (state_ == State::Idle)
        startMetadata(State::Checking);
}

void ClientUpdater::startMetadata(State state) {
    state_ = state;
    auto request = requestFor(releasesApi);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    metadata_.clear();
    transferError_.clear();
    setStatus(state == State::Checking ? QStringLiteral("正在检查更新…") : QStringLiteral("正在复核发布信息…"));
    reply_ = network_.get(request);
    reply_->setReadBufferSize(64 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, [this] { readMetadata(); });
    connect(reply_, &QNetworkReply::finished, this, [this] { finishMetadata(); });
    deadline_.start(20000);
}

void ClientUpdater::readMetadata() {
    const auto bytes = reply_->readAll();
    if (bytes.size() > maximumMetadataSize - metadata_.size()) {
        abortTransfer(QStringLiteral("更新信息超出大小限制。"));
        return;
    }
    metadata_.append(bytes);
}

void ClientUpdater::finishMetadata() {
    deadline_.stop();
    if (transferError_.isEmpty())
        readMetadata();
    const auto reply = std::exchange(reply_, nullptr);
    reply->deleteLater();
    if (!transferError_.isEmpty()) {
        fail(transferError_);
        return;
    }
    const auto status = statusCode(*reply);
    if (status == 404) {
        fail(QStringLiteral("暂无已发布的版本。"));
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        fail(status == 403 || status == 429
                 ? QStringLiteral("GitHub 暂时限制了请求，请稍后重试。")
                 : QStringLiteral("无法连接 GitHub 检查更新，请检查网络后重试。"));
        return;
    }
    const auto latest = readUpdateRelease(metadata_, QCoreApplication::applicationVersion(), portable_);
    if (!latest) {
        fail(QStringLiteral("GitHub 返回的发布信息无法识别。"));
        return;
    }
    if (state_ == State::Rechecking) {
        if (!latest->newer || !release_ || !latest->matchesAsset(*release_)) {
            fail(QStringLiteral("发布信息已变化，请重新检查更新。"));
            return;
        }
        startDownload();
        return;
    }

    state_ = State::Idle;
    if (!latest->newer) {
        release_.reset();
        notes_->clear();
        setStatus(QStringLiteral("当前已是最新版本（%1）。").arg(QCoreApplication::applicationVersion()));
        return;
    }
    release_ = latest;
    notes_->setPlainText(latest->notes);
    const bool usable = !latest->assetUrl.isEmpty() &&
                        QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(helperName));
    const auto message = usable
        ? QStringLiteral("发现新版本 %1。更新会结束当前会话并重启客户端。").arg(latest->version)
        : QStringLiteral("发现新版本 %1。当前客户端无法自动安装此版本。").arg(latest->version);
    setStatus(message);
    if (manual_ || (usable && lastOffered_ != latest->tag)) {
        lastOffered_ = latest->tag;
        dialog_->setAttribute(Qt::WA_ShowWithoutActivating, !manual_);
        dialog_->show();
    }
}

void ClientUpdater::startDownload() {
    staging_ = std::make_unique<QTemporaryDir>(QDir::temp().filePath("evp-update-XXXXXX"));
    if (!staging_->isValid()) {
        fail(QStringLiteral("无法创建更新临时目录。"));
        return;
    }
    download_.setFileName(staging_->filePath(updateAssetName(portable_)));
    if (!download_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(QStringLiteral("无法保存更新文件。"));
        return;
    }
    state_ = State::Downloading;
    received_ = 0;
    prefix_.clear();
    hash_.reset();
    transferError_.clear();
    auto request = requestFor(release_->assetUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    setStatus(QStringLiteral("正在下载 %1…").arg(portable_ ? QStringLiteral("便携版") : QStringLiteral("安装包")));
    reply_ = network_.get(request);
    reply_->setReadBufferSize(256 * 1024);
    connect(reply_, &QNetworkReply::redirected, this, [this](const QUrl& url) {
        if (trustedUpdateRedirect(reply_->url().resolved(url)))
            emit reply_->redirectAllowed();
        else
            abortTransfer(QStringLiteral("更新下载地址无法验证。"));
    });
    connect(reply_, &QNetworkReply::readyRead, this, [this] { readPayload(); });
    connect(reply_, &QNetworkReply::finished, this, [this] { finishDownload(); });
    deadline_.start(downloadIdleTimeoutMs);
}

void ClientUpdater::readPayload() {
    const auto bytes = reply_->readAll();
    if (statusCode(*reply_) != 200 || bytes.isEmpty())
        return;
    if (bytes.size() > release_->size - received_) {
        abortTransfer(QStringLiteral("更新文件大小与发布记录不符。"));
        return;
    }
    if (download_.write(bytes) != bytes.size()) {
        abortTransfer(QStringLiteral("写入更新文件失败。"));
        return;
    }
    prefix_.append(bytes.first(qMin<qsizetype>(2 - prefix_.size(), bytes.size())));
    hash_.addData(bytes);
    received_ += bytes.size();
    deadline_.start(downloadIdleTimeoutMs);
    progress_->setValue(static_cast<int>(received_ * 100 / release_->size));
}

void ClientUpdater::finishDownload() {
    if (transferError_.isEmpty())
        readPayload();
    deadline_.stop();
    const auto reply = std::exchange(reply_, nullptr);
    reply->deleteLater();
    if (!transferError_.isEmpty()) {
        fail(transferError_);
        return;
    }
    if (reply->error() != QNetworkReply::NoError || statusCode(*reply) != 200) {
        fail(QStringLiteral("更新下载失败，请检查网络后重试。"));
        return;
    }
    if (received_ != release_->size || prefix_ != "MZ" || hash_.result() != release_->sha256 || !download_.flush()) {
        fail(QStringLiteral("更新文件完整性校验失败。"));
        return;
    }
    download_.close();
    applyUpdate();
}

void ClientUpdater::applyUpdate() {
    const auto helper = staging_->filePath(helperName);
    const auto runtime = QDir(QCoreApplication::applicationDirPath());
    const auto origin = portable_ ? launcherPath_ : QCoreApplication::applicationFilePath();
    bool validPid = false;
    const auto pid = launcherPid_.toUInt(&validPid);
    if (portable_ && (!validPid || !pid || !QFileInfo(origin).isAbsolute() || !QFileInfo(origin).isFile())) {
        fail(QStringLiteral("无法识别当前便携程序的位置。"));
        return;
    }
    const QStringList args{
        portable_ ? "--portable" : "--setup",
        QString::number(QCoreApplication::applicationPid()),
        portable_ ? launcherPid_ : QStringLiteral("0"),
        QDir::toNativeSeparators(origin), QDir::toNativeSeparators(download_.fileName()),
        QString::fromLatin1(release_->sha256.toHex()), release_->version};
    if (!QFile::copy(runtime.filePath(helperName), helper) || !QProcess::startDetached(helper, args, staging_->path())) {
        fail(QStringLiteral("无法启动更新程序。"));
        return;
    }
    staging_->setAutoRemove(false);
    state_ = State::Restarting;
    setStatus(QStringLiteral("更新即将应用，客户端将重启。"));
    QCoreApplication::quit();
}

void ClientUpdater::setStatus(const QString& text) {
    status_->setText(text);
    notes_->setVisible(release_ && !notes_->document()->isEmpty());
    const bool checking = state_ == State::Checking || state_ == State::Rechecking;
    const bool downloading = state_ == State::Downloading;
    progress_->setVisible(checking || downloading);
    if (downloading)
        progress_->setRange(0, 100);
    else
        progress_->setRange(0, 0);
    progress_->setValue(0);
    const auto canInstall = release_ && !release_->assetUrl.isEmpty() &&
        QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(helperName));
    install_->setVisible(canInstall && state_ == State::Idle);
    close_->setText(downloading ? QStringLiteral("取消下载") : QStringLiteral("关闭"));
    close_->setEnabled(state_ != State::Restarting);
}

void ClientUpdater::abortTransfer(const QString& error) {
    transferError_ = error;
    if (reply_ && !reply_->isFinished())
        reply_->abort();
}

void ClientUpdater::fail(const QString& message) {
    const bool preserveOffer = state_ == State::Downloading || (state_ == State::Rechecking && !transferError_.isEmpty());
    state_ = State::Idle;
    download_.close();
    staging_.reset();
    if (!preserveOffer) {
        release_.reset();
        notes_->clear();
    }
    setStatus(message);
}

void ClientUpdater::closeDialog() {
    manual_ = false;
    if (state_ == State::Downloading || state_ == State::Rechecking)
        abortTransfer(QStringLiteral("已取消更新。"));
}

}
