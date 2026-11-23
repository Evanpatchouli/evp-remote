#include "client_updater.h"

#include "ui/action_button.h"
#include "ui/home_window.h"
#include "ui/visual_style.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QScrollBar>
#include <QScreen>
#include <QStyle>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QtMath>
#include <QVBoxLayout>

#include <utility>

namespace evp {
namespace {

constexpr qsizetype maximumMetadataSize = 256 * 1024;
constexpr int checkIntervalMs = 15 * 60 * 1000;
constexpr int downloadIdleTimeoutMs = 60 * 1000;
const auto releasesApi = QUrl("https://api.github.com/repos/Evanpatchouli/evp-remote/releases/latest");
const auto helperName = QStringLiteral("EvpRemoteUpdate.exe");
constexpr int dialogWidth = 428;
constexpr int releaseDialogWidth = 500;
constexpr int dialogSideMargin = 20;
constexpr int notesHorizontalInset = 13; // Padding plus border on each side.
constexpr int notesVerticalInset = 10;
constexpr int minimumNotesHeight = 64;
constexpr int maximumNotesHeight = 246;

class NotesImageHandler final : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    using QObject::QObject;
    QSizeF intrinsicSize(QTextDocument*, int, const QTextFormat&) override { return {0, 0}; }
    void drawObject(QPainter*, const QRectF&, QTextDocument*, int, const QTextFormat&) override {}
};

void formatReleaseNotes(QTextDocument& document, const QString& markdown) {
    document.setMarkdown(markdown, {QTextDocument::MarkdownDialectGitHub, QTextDocument::MarkdownNoHTML});
    QTextCursor cursor(&document);
    cursor.beginEditBlock();
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        auto format = block.blockFormat();
        if (format.hasProperty(QTextFormat::BlockCodeLanguage))
            continue;
        cursor.setPosition(block.position());
        const int heading = format.headingLevel();
        format.setLineHeight(128, QTextBlockFormat::ProportionalHeight);
        format.setTopMargin(heading && block != document.begin() ? 14 : 0);
        format.setBottomMargin(heading || block.textList() ? 7 : 10);
        cursor.setBlockFormat(format);
        if (heading && block.length() > 1) {
            QTextCharFormat title;
            title.setFontWeight(QFont::DemiBold);
            title.setProperty(QTextFormat::FontSizeAdjustment, heading <= 2 ? 1 : 0);
            cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(title);
        }
    }
    cursor.endEditBlock();
}

int dialogHeightForWidth(QDialog& dialog, int width) {
    const int wrapped = dialog.layout()->totalHeightForWidth(width);
    return wrapped >= 0 ? wrapped : dialog.layout()->sizeHint().height();
}

void keepDialogOnScreen(QDialog& dialog) {
    const auto available = dialog.screen()->availableGeometry();
    const auto frame = dialog.frameGeometry();
    const QPoint topLeft(
        qBound(available.left(), frame.left(), qMax(available.left(), available.right() - frame.width() + 1)),
        qBound(available.top(), frame.top(), qMax(available.top(), available.bottom() - frame.height() + 1)));
    if (topLeft != frame.topLeft())
        dialog.move(dialog.pos() + topLeft - frame.topLeft());
}

int statusCode(const QNetworkReply& reply) {
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

QNetworkRequest requestFor(const QUrl& url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "EVP-Remote/" + QCoreApplication::applicationVersion().toUtf8());
    return request;
}

}

ClientUpdater::ClientUpdater(HomeWindow& home, const QUrl& releaseEndpoint)
    : QObject(&home),
      dialog_(new QDialog(&home)),
      releaseEndpoint_(releaseEndpoint.isEmpty() ? releasesApi : releaseEndpoint),
      launcherPath_(qEnvironmentVariable("EVP_PORTABLE_LAUNCHER_PATH")),
      launcherPid_(qEnvironmentVariable("EVP_PORTABLE_LAUNCHER_PID")),
      portable_(!launcherPath_.isEmpty() || !launcherPid_.isEmpty()) {
    dialog_->setObjectName("updateDialog");
    dialog_->setWindowTitle(QStringLiteral("检查更新 · EVP 远程"));
    dialog_->setFixedWidth(dialogWidth);
    native_ui::setThemedStyle(*dialog_, QStringLiteral(
        "QDialog { background: #ffffff/#25272b@surface; }"
        "QLabel { color: #182844/#e6e8ec@foreground; background: transparent; }"
        "QLabel#updateDetail, QLabel#updateVersionCaption { color: #526684/#b7c0cf@muted; }"
        "QLabel#updateLatestVersion { color: #2356b3/#b7d0ff@accentText; }"
        "QFrame#updateVersions { background: #f5f8fd/#222427@surface; border: 1px solid #e0e8f3/#343942@stroke; border-radius: 8px; }"
        "QTextBrowser { background: #f8faff/#202226@raised; color: #263955/#e6e8ec@foreground; border: 1px solid #e0e8f3/#343942@stroke;"
        " border-radius: 8px; padding: %1px %2px; }"
        "QProgressBar { min-height: 6px; max-height: 6px; border: none; border-radius: 3px; background: #e7eef9/#292c31@raised; }"
        "QProgressBar::chunk { border-radius: 3px; background: #3475e4/#739ff1@accent; }"
        "QPushButton { background: #edf4ff/#303844@raised; color: #214c98/#b7d0ff@accentText; border: 1px solid #b2c8eb/#536783@stroke; padding: 0 12px; outline: none; }"
        "QPushButton:hover { background: #e0edff/#344153@accentHoverTint; }"
        "QPushButton:focus { border-color: #3475e4/#739ff1@accentMark; }"
        "QPushButton#installUpdate { background: #2865d5/#2355ad@accent; border-color: #2865d5/#739ff1@accentMark; color: #ffffff/#eef0f5@onAccent; }"
        "QPushButton#installUpdate:hover { background: #2058c2/#2a60bc@accentHover; }"
        "QPushButton#installUpdate:focus { border: 2px solid #d3e2ff/#b7d0ff@onAccent; }"
        "QPushButton:disabled, QPushButton#installUpdate:disabled {"
        " background: #f4f7fc/#24262a@surface; color: #667893/#9da7b8@muted; border-color: #d9e3f1/#3d434d@stroke; }")
        .arg(notesVerticalInset - 1).arg(notesHorizontalInset - 1));
    auto* layout = new QVBoxLayout(dialog_);
    layout->setContentsMargins(dialogSideMargin, 18, dialogSideMargin, 16);
    layout->setSpacing(14);
    auto* header = new QHBoxLayout;
    header->setSpacing(12);
    mark_ = native_ui::label({}, "updateMark", 13);
    mark_->setAlignment(Qt::AlignCenter);
    mark_->setFixedSize(44, 44);
    native_ui::onThemeChanged(*mark_, [this] { updateMark(); });
    header->addWidget(mark_, 0, Qt::AlignTop);
    auto* summary = new QVBoxLayout;
    summary->setSpacing(4);
    status_ = native_ui::label({}, "updateStatus", 18, QFont::DemiBold);
    detail_ = native_ui::label({}, "updateDetail", 13);
    detail_->setWordWrap(true);
    summary->addWidget(status_);
    summary->addWidget(detail_);
    header->addLayout(summary, 1);
    layout->addLayout(header);

    versions_ = new QFrame;
    versions_->setObjectName("updateVersions");
    auto* versionRow = new QHBoxLayout(versions_);
    versionRow->setContentsMargins(12, 8, 12, 8);
    const auto addVersion = [versionRow](const QString& caption, const char* name) {
        auto* column = new QVBoxLayout;
        column->setSpacing(3);
        column->addWidget(native_ui::label(caption, "updateVersionCaption", 12));
        auto* version = native_ui::label({}, name, 15, QFont::DemiBold);
        column->addWidget(version);
        versionRow->addLayout(column, 1);
        return version;
    };
    currentVersion_ = addVersion(QStringLiteral("当前版本"), "updateCurrentVersion");
    latestVersion_ = addVersion(QStringLiteral("最新版本"), "updateLatestVersion");
    layout->addWidget(versions_);

    notes_ = new QTextBrowser;
    notes_->setObjectName("updateReleaseNotes");
    notes_->document()->setIndentWidth(20);
    notes_->document()->documentLayout()->registerHandler(QTextFormat::ImageObject, new NotesImageHandler(notes_));
    notes_->setOpenLinks(false);
    notes_->setOpenExternalLinks(false);
    notes_->setAccessibleName(QStringLiteral("更新说明"));
    notes_->setFont(native_ui::uiFont(14));
    layout->addWidget(notes_);
    progress_ = new QProgressBar;
    progress_->setAccessibleName(QStringLiteral("更新下载进度"));
    progress_->setTextVisible(false);
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
    connect(install_, &QPushButton::clicked, this, [this] {
        if (canInstall())
            startMetadata(State::Rechecking);
        else
            check(true);
    });
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
    setStatus(Appearance::Activity, QStringLiteral("正在检查更新…"));
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
    manual_ |= manual;
    if (state_ == State::Idle)
        startMetadata(State::Checking);
    if (manual) {
        dialog_->setAttribute(Qt::WA_ShowWithoutActivating, false);
        dialog_->show();
        dialog_->raise();
        dialog_->activateWindow();
    }
}

void ClientUpdater::startMetadata(State state) {
    state_ = state;
    auto request = requestFor(releaseEndpoint_);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    metadata_.clear();
    transferError_.clear();
    setStatus(Appearance::Activity, state == State::Checking
        ? QStringLiteral("正在检查更新…") : QStringLiteral("正在复核发布信息…"));
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
        setStatus(Appearance::Current, QStringLiteral("已是最新版本"),
                  QStringLiteral("当前版本 %1").arg(QCoreApplication::applicationVersion()));
        return;
    }
    release_ = latest;
    formatReleaseNotes(*notes_->document(), latest->notes);
    notes_->moveCursor(QTextCursor::Start);
    const bool usable = canInstall();
    const auto message = usable
        ? QStringLiteral("更新会结束当前会话并重启客户端。")
        : QStringLiteral("当前客户端无法自动安装此版本。");
    setStatus(Appearance::Offer, QStringLiteral("发现新版本"), message);
    if (manual_ || (usable && lastOffered_ != latest->tag)) {
        lastOffered_ = latest->tag;
        dialog_->setAttribute(Qt::WA_ShowWithoutActivating, !manual_);
        dialog_->show();
        keepDialogOnScreen(*dialog_);
        notes_->verticalScrollBar()->setValue(0);
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
    setStatus(Appearance::Activity,
              QStringLiteral("正在下载 %1…").arg(portable_ ? QStringLiteral("便携版") : QStringLiteral("安装包")),
              QStringLiteral("已下载 0%"));
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
    const auto percent = static_cast<int>(received_ * 100 / release_->size);
    progress_->setValue(percent);
    detail_->setText(QStringLiteral("已下载 %1%").arg(percent));
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
    setStatus(Appearance::Activity, QStringLiteral("正在应用更新…"), QStringLiteral("客户端即将重启。"));
    QCoreApplication::quit();
}

bool ClientUpdater::canInstall() const {
    return release_ && !release_->assetUrl.isEmpty() &&
        QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(helperName));
}

void ClientUpdater::updateMark() {
    using native_ui::Symbol;
    using native_ui::themeColor;
    struct Mark { Symbol symbol; QColor ink; QColor background; };
    const auto blueInk = themeColor("#2865d5", "#aec9fb");
    const auto blueBackground = themeColor("#edf4ff", "#283b56");
    Mark mark{Symbol::Refresh, blueInk, blueBackground};
    if (appearance_ == Appearance::Current)
        mark = {Symbol::Check, themeColor("#198064", "#a2d9bd"), themeColor("#e7f5ef", "#243a31")};
    else if (appearance_ == Appearance::Offer)
        mark = {Symbol::Download, blueInk, blueBackground};
    else if (appearance_ == Appearance::Warning)
        mark = {Symbol::Warning, themeColor("#9e5a14", "#edb782"), themeColor("#fff3e1", "#3d3026")};
    mark_->setPixmap(native_ui::symbolPixmap(mark.symbol, mark.ink, 24));
    mark_->setStyleSheet(QStringLiteral("background: %1; border-radius: 11px;").arg(mark.background.name()));
}

void ClientUpdater::setStatus(Appearance appearance, const QString& heading, const QString& detail) {
    appearance_ = appearance;
    updateMark();
    status_->setText(heading);
    detail_->setText(detail);
    detail_->setVisible(!detail.isEmpty());

    const bool showRelease = release_ && state_ == State::Idle;
    versions_->setVisible(showRelease);
    if (showRelease) {
        currentVersion_->setText(QCoreApplication::applicationVersion());
        latestVersion_->setText(release_->version);
    }
    const bool showNotes = showRelease && !notes_->document()->isEmpty();
    const auto available = dialog_->screen()->availableGeometry();
    const int width = qMin(showNotes ? releaseDialogWidth : dialogWidth, available.width() - 16);
    dialog_->setFixedWidth(width);
    notes_->setVisible(showNotes);
    const bool checking = state_ == State::Checking || state_ == State::Rechecking;
    const bool downloading = state_ == State::Downloading;
    progress_->setVisible(checking || downloading);
    if (downloading)
        progress_->setRange(0, 100);
    else
        progress_->setRange(0, 0);
    progress_->setValue(0);
    const bool ready = canInstall() && state_ == State::Idle;
    const bool retry = appearance == Appearance::Warning && state_ == State::Idle;
    install_->setVisible(ready || retry);
    install_->setText(ready ? (retry ? QStringLiteral("重试更新") : QStringLiteral("下载并更新"))
                            : QStringLiteral("重新检查"));
    close_->setText(downloading ? QStringLiteral("取消下载")
                               : ready ? QStringLiteral("稍后再说") : QStringLiteral("关闭"));
    close_->setEnabled(state_ != State::Restarting);
    if (showNotes) {
        notes_->setFixedHeight(minimumNotesHeight);
        const int chromeHeight = dialogHeightForWidth(*dialog_, width) - minimumNotesHeight;
        const int titleBar = dialog_->style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, dialog_);
        const int limit = qBound(minimumNotesHeight, available.height() - chromeHeight - titleBar - 16, maximumNotesHeight);
        const int textWidth = width - 2 * (dialogSideMargin + notesHorizontalInset);
        auto* document = notes_->document();
        document->setTextWidth(textWidth);
        const auto contentHeight = [&] { return qCeil(document->size().height()) + 2 * notesVerticalInset; };
        if (contentHeight() > limit) {
            // Keep wrapped lines clear of the vertical scrollbar in a capped viewport.
            const int scrollWidth = notes_->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, notes_);
            document->setTextWidth(textWidth - scrollWidth);
        }
        notes_->setFixedHeight(qBound(minimumNotesHeight, contentHeight(), limit));
    }
    dialog_->layout()->activate();
    dialog_->resize(width, dialogHeightForWidth(*dialog_, width));
}

void ClientUpdater::abortTransfer(const QString& error) {
    transferError_ = error;
    if (reply_ && !reply_->isFinished())
        reply_->abort();
}

void ClientUpdater::fail(const QString& message) {
    const bool checking = state_ == State::Checking;
    const bool preserveOffer = state_ == State::Downloading || (state_ == State::Rechecking && !transferError_.isEmpty());
    state_ = State::Idle;
    download_.close();
    staging_.reset();
    if (!preserveOffer) {
        release_.reset();
        notes_->clear();
    }
    setStatus(Appearance::Warning,
              checking ? QStringLiteral("检查更新未完成") : QStringLiteral("更新未完成"), message);
}

void ClientUpdater::closeDialog() {
    manual_ = false;
    if (state_ == State::Downloading || state_ == State::Rechecking)
        abortTransfer(QStringLiteral("已取消更新。"));
}

}

#include "client_updater.moc"
