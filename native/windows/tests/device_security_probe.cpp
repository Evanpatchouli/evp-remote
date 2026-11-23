#include "client_updater.h"
#include "controller.h"
#include "device_probe.h"
#include "devices/device_identity.h"
#include "ui/focus_style.h"
#include "ui/select_field.h"
#include "ui/system_tray.h"
#include "ui/theme.h"
#include "update_release.h"
#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QHoverEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QPainter>
#include <QRadioButton>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebSocket>
#include <QWebSocketServer>
#include <Windows.h>
#include <dpapi.h>

namespace {

struct LoopbackBroker {
    QList<QJsonObject> frames;
    int connections = 0, closed = 0;
    QWebSocketServer server{"Settings probe", QWebSocketServer::NonSecureMode};

    bool listen() {
        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [this] {
            auto* socket = server.nextPendingConnection();
            socket->setParent(&server);
            ++connections;
            QObject::connect(socket, &QWebSocket::disconnected, socket, [this] { ++closed; });
            QObject::connect(socket, &QWebSocket::textMessageReceived, socket, [this](const QString& text) {
                frames.append(QJsonDocument::fromJson(text.toUtf8()).object());
            });
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
    QUrl url() const {
        auto result = server.serverUrl();
        result.setPath("/");
        return result;
    }
};

auto lockAgainstReplacement(const QString& path) {
    const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
    const auto handle = CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto close = [](void* value) { CloseHandle(value); };
    return std::unique_ptr<void, decltype(close)>(handle == INVALID_HANDLE_VALUE ? nullptr : handle, close);
}

bool writeLegacyProfile(const QString& path, const evp::DeviceIdentity& owner, const QJsonObject& state) {
    auto plain = owner.privateKey() + QJsonDocument(state).toJson(QJsonDocument::Compact);
    QByteArray context("evp-device-store-v1");
    DATA_BLOB source{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB entropy{static_cast<DWORD>(context.size()), reinterpret_cast<BYTE*>(context.data())};
    DATA_BLOB encrypted{};
    const bool ok = CryptProtectData(&source, L"EVP Remote devices", &entropy, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &encrypted);
    SecureZeroMemory(plain.data(), static_cast<SIZE_T>(plain.size()));
    if (!ok)
        return false;
    const auto cleanup = qScopeGuard([&] { LocalFree(encrypted.pbData); });
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(reinterpret_cast<const char*>(encrypted.pbData), encrypted.cbData) == encrypted.cbData;
}

QStringList trustedDeviceIds(const QListWidget& list) {
    QStringList ids;
    for (int row = 0; row < list.count(); ++row)
        ids.append(list.item(row)->data(Qt::UserRole).toString());
    return ids;
}

void verifyElevation(const QPixmap& layer, const QRect& panel) {
    const auto pixels = layer.toImage().convertToFormat(QImage::Format_ARGB32);
    const auto ratio = layer.devicePixelRatio();
    const auto alpha = [&](int column, int row) {
        return qAlpha(pixels.pixel(qRound(column * ratio), qRound(row * ratio)));
    };
    for (const auto corner : {pixels.rect().topLeft(), pixels.rect().topRight(),
                              pixels.rect().bottomLeft(), pixels.rect().bottomRight()})
        QCOMPARE(qAlpha(pixels.pixel(corner)), 0);
    for (const auto corner : {panel.topLeft(), panel.topRight(), panel.bottomLeft(), panel.bottomRight()})
        QVERIFY(alpha(corner.x(), corner.y()) < 80);
    QVERIFY(layer.deviceIndependentSize().height() - panel.bottom() > 6);
    const auto nearShadow = alpha(panel.center().x(), panel.bottom() + 2);
    QVERIFY(nearShadow > 0 && nearShadow < 80);
    QVERIFY(alpha(panel.center().x(), panel.bottom() + 6) < nearShadow);
}

class DeviceSecurityProbe : public QObject {
    Q_OBJECT

private slots:
    void bindsProofToPeerAttemptNonceAndPayload();
    void pausesOnUnwritableSettingsAcrossRestarts();
    void persistsLocalDeviceNamesAndOrder();
    void rejectsUnwritableDeviceMetadata_data();
    void rejectsUnwritableDeviceMetadata();
    void lockedProfileRejectsFallbackJoin();
    void retriesFailedForgetWithoutEditing();
    void committedAddressSurvivesHeldMarker();
    void settingsEditorDiscardsDrafts();
    void appearanceFollowsAndPersistsTheme();
    void appearanceCustomizesActivePaletteAndFont();
    void guestJoinsWithAddressOnly();
    void storedKeyIsScopedToAddress();
    void readsLegacyConnectionSettings_data();
    void readsLegacyConnectionSettings();
    void assistanceTabsAndScrollableDeviceList();
    void cleanTabFocus();
    void cleanSidebarFocus_data();
    void cleanSidebarFocus();
    void cleanDeviceListFocus();
    void trustedDeviceMenuActions();
    void trustedDevicePinHover();
    void cleanCheckBoxFocus();
    void checkBoxCursorFollowsEnabledState();
    void themedLifetimeSelectors_data();
    void themedLifetimeSelectors();
    void floatingStopControl();
    void trayKeepsSharingControlsAvailable();
    void onscreenPopupOpening();
    void selectsVerifiedUpdateAssets();
    void rendersUpdateDialog_data();
    void rendersUpdateDialog();
};

void DeviceSecurityProbe::trayKeepsSharingControlsAvailable() {
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        QSKIP("The system tray is unavailable in this Windows session.");
    const auto quitsOnLastWindow = QApplication::quitOnLastWindowClosed();
    const auto restore = qScopeGuard([=] { QApplication::setQuitOnLastWindowClosed(quitsOnLastWindow); });
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    native_ui::installSystemTray(window);
    auto* tray = window.findChild<QSystemTrayIcon*>();
    auto* open = window.findChild<QAction*>("trayOpen");
    auto* quit = window.findChild<QAction*>("trayQuit");
    QVERIFY(tray && tray->isVisible() && open && quit);
    QCOMPARE(open->text(), QStringLiteral("打开"));
    QCOMPARE(quit->text(), QStringLiteral("退出"));
    const auto icon = tray->icon().pixmap(16, 16).toImage();
    QVERIFY(!icon.isNull() && icon.pixelColor(0, 0).alpha() == 0);
    QVERIFY(!QApplication::quitOnLastWindowClosed());
    window.show();
    QVERIFY(window.close());
    QVERIFY(!window.isVisible() && tray->isVisible());
    open->trigger();
    QVERIFY(window.isVisible());

    QVERIFY(window.setHostBusy(true));
    window.setHostActive(true);
    auto* stop = window.findChild<QPushButton*>("hostEnd");
    QVERIFY(stop && stop->isVisible());
    QPushButton* floating = nullptr;
    for (auto* top : QApplication::topLevelWidgets()) {
        if (auto* button = top->findChild<QPushButton*>("trustedShareStop")) {
            floating = button;
            break;
        }
    }
    QVERIFY(floating && !floating->isVisible());
    QVERIFY(window.close());
    QVERIFY(!window.isVisible() && floating->isVisible());
    QSignalSpy stopRequested(&window, &HomeWindow::hostEndRequested);
    floating->click();
    QCOMPARE(stopRequested.count(), 1);
    tray->activated(QSystemTrayIcon::Trigger);
    QVERIFY(window.isVisible() && !floating->isVisible());
    QVERIFY(window.setHostBusy(false));
}

void DeviceSecurityProbe::selectsVerifiedUpdateAssets() {
    const QString tag = "v0.10.0";
    const auto makeAsset = [&tag](bool portable, const QString& host = "github.com") {
        const auto name = evp::updateAssetName(portable);
        return QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", 1024},
                           {"digest", QStringLiteral("sha256:") + QString(64, 'a')},
                           {"browser_download_url", QStringLiteral("https://%1/Evanpatchouli/evp-remote/releases/download/%2/%3")
                               .arg(host, tag, name)}};
    };
    QJsonObject release{{"tag_name", tag}, {"body", "Update notes"}, {"draft", false}, {"prerelease", false}};
    const auto read = [&](bool portable, const QString& current = "0.2.0") {
        return evp::readUpdateRelease(QJsonDocument(release).toJson(QJsonDocument::Compact), current, portable);
    };
    release.insert("assets", QJsonArray{makeAsset(false), makeAsset(true)});
    for (const bool portable : {false, true}) {
        const auto result = read(portable);
        QVERIFY(result && result->newer && result->assetUrl.path().endsWith(evp::updateAssetName(portable)));
        QCOMPARE(result->version, QString("0.10.0"));
        QCOMPARE(result->size, 1024);
        QCOMPARE(result->sha256, QByteArray(32, '\xaa'));
    }
    const auto alreadyCurrent = read(false, "0.10.0");
    QVERIFY(alreadyCurrent && !alreadyCurrent->newer);
    const auto fromPreview = read(false, "0.10.0-beta.1");
    QVERIFY(fromPreview && fromPreview->newer);

    auto missingDigest = makeAsset(true);
    missingDigest.remove("digest");
    release.insert("assets", QJsonArray{makeAsset(false, "github.com.example.org"), missingDigest});
    QVERIFY(read(false) && read(false)->assetUrl.isEmpty());
    QVERIFY(read(true) && read(true)->assetUrl.isEmpty());
    release.insert("prerelease", true);
    QVERIFY(!read(false));
    QVERIFY(evp::trustedUpdateRedirect(QUrl("https://release-assets.githubusercontent.com/opaque?token=test")));
    QVERIFY(!evp::trustedUpdateRedirect(QUrl("http://release-assets.githubusercontent.com/opaque")));
    QVERIFY(!evp::trustedUpdateRedirect(QUrl("https://release-assets.githubusercontent.com.example/opaque")));
}

void DeviceSecurityProbe::rendersUpdateDialog_data() {
    QTest::addColumn<bool>("hasUpdate");
    QTest::addColumn<bool>("longNotes");
    QTest::newRow("current") << false << false;
    QTest::newRow("available") << true << false;
    QTest::newRow("scrollable-notes") << true << true;
}

void DeviceSecurityProbe::rendersUpdateDialog() {
    QFETCH(bool, hasUpdate);
    QFETCH(bool, longNotes);
    QTcpServer releases;
    QVERIFY(releases.listen(QHostAddress::LocalHost));
    const QString tag = "0.2.1";
    const auto assetName = evp::updateAssetName(false);
    const QJsonObject asset{{"name", assetName}, {"state", "uploaded"}, {"size", 1024},
        {"digest", QStringLiteral("sha256:") + QString(64, 'a')},
        {"browser_download_url", QStringLiteral("https://github.com/Evanpatchouli/evp-remote/releases/download/%1/%2")
             .arg(tag, assetName)}};
    auto notesText = QStringLiteral("## Windows 客户端\n\n- 安装向导支持选择目录。\n- 检查更新界面更紧凑。");
    if (longNotes) {
        notesText += QStringLiteral("\n- 这是一条较长的说明，用于验证说明窗口的空间和滚动位置。").repeated(25);
        notesText += QStringLiteral("\n\n```powershell\nGet-Date\nGet-Location\n```");
    }
    const auto body = QJsonDocument(QJsonObject{{"tag_name", tag}, {"draft", false}, {"prerelease", false},
        {"body", notesText},
        {"assets", QJsonArray{asset}}}).toJson(QJsonDocument::Compact);
    const auto response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\n\r\n" + body;
    connect(&releases, &QTcpServer::newConnection, &releases, [&] {
        auto* socket = releases.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [socket, &response, request = QByteArray{}]() mutable {
            request.append(socket->readAll());
            if (request.contains("\r\n\r\n")) {
                socket->write(response);
                socket->disconnectFromHost();
            }
        });
    });

    const auto previousVersion = QCoreApplication::applicationVersion();
    const auto restore = qScopeGuard([&] { QCoreApplication::setApplicationVersion(previousVersion); });
    QCoreApplication::setApplicationVersion(hasUpdate ? QStringLiteral("0.2.0") : tag);
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/").arg(releases.serverPort()));
    evp::ClientUpdater updater(window, endpoint);
    auto* dialog = window.findChild<QDialog*>("updateDialog");
    QVERIFY(dialog);
    dialog->setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    const device_probe::Ui ui(window);
    ui.button("settingsNavigation")->click();
    ui.button("settingsAboutNavigation")->click();
    QTest::mouseClick(ui.button("checkForUpdates"), Qt::LeftButton);
    const auto* status = window.findChild<QLabel*>("updateStatus");
    const auto* detail = window.findChild<QLabel*>("updateDetail");
    QVERIFY(dialog && status && detail);
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), hasUpdate ? QStringLiteral("发现新版本")
                                                       : QStringLiteral("已是最新版本"), 15000);
    QCOMPARE(window.findChild<QWidget*>("updateVersions")->isVisible(), hasUpdate);
    if (hasUpdate) {
        QCOMPARE(window.findChild<QLabel*>("updateLatestVersion")->text(), tag);
        QVERIFY(dialog->height() <= (longNotes ? 480 : 380));
        auto* notes = dialog->findChild<QTextBrowser*>("updateReleaseNotes");
        QVERIFY(notes && notes->isVisible());
        const auto title = notes->document()->firstBlock();
        QCOMPARE(title.blockFormat().headingLevel(), 2);
        QCOMPARE(title.next().blockFormat().lineHeightType(), int(QTextBlockFormat::ProportionalHeight));
        QVERIFY(title.next().blockFormat().bottomMargin() > 0);
        if (longNotes) {
            QTRY_VERIFY(notes->verticalScrollBar()->maximum() > 0);
            QCOMPARE(notes->verticalScrollBar()->value(), notes->verticalScrollBar()->minimum());
            QCOMPARE(notes->horizontalScrollBar()->maximum(), 0);
            QVERIFY(dialog->frameGeometry().height() <= dialog->screen()->availableGeometry().height());
            const auto command = notes->document()->find("Get-Date").block();
            QVERIFY(command.isValid());
            QCOMPARE(command.blockFormat().bottomMargin(), 0.0);
            QCOMPARE(command.next().text(), QStringLiteral("Get-Location"));
        }
        auto* install = dialog->findChild<QPushButton*>("installUpdate");
        QVERIFY(install);
        if (install->isVisible()) {
            QApplication::setActiveWindow(dialog);
            install->clearFocus();
            const auto unfocused = install->grab().toImage();
            install->setFocus(Qt::TabFocusReason);
            QVERIFY(install->hasFocus());
            QVERIFY2(install->grab().toImage() != unfocused, "The update button needs a visible focus border.");
        }
    } else {
        QVERIFY(detail->text().contains(QCoreApplication::applicationVersion()));
        QVERIFY(dialog->height() <= 170);
    }
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty()) {
        const auto file = longNotes ? "update-long.png" : hasUpdate ? "update-available.png" : "update-current.png";
        QVERIFY(dialog->grab().save(QDir(preview).filePath(file)));
    }
}

void DeviceSecurityProbe::bindsProofToPeerAttemptNonceAndPayload() {
    evp::DeviceIdentity host, viewer;
    QVERIFY(host.load({}));
    QVERIFY(viewer.load({}));
    const auto hostId = evp::deviceId(host.publicKey());
    const auto viewerId = evp::deviceId(viewer.publicKey());
    QVERIFY(!hostId.isEmpty() && !viewerId.isEmpty() && hostId != viewerId);
    const auto token = [] { return evp::encodeDeviceBytes(evp::deviceRandom()); };
    const auto attempt = token();
    const auto nonce = token();
    const auto payload = [](const QByteArray& fingerprint) {
        const auto sdp = QString::fromLatin1("v=0\r\na=fingerprint:sha-256 " + fingerprint.toHex(':') + "\r\n");
        const QJsonObject description{{"type", "offer"}, {"sdp", sdp}};
        return evp::encodeDeviceBytes(QJsonDocument(QJsonObject{{"description", description}}).toJson(QJsonDocument::Compact));
    };
    const auto original = payload(evp::deviceRandom());
    const auto frame = [&](const QString& frameAttempt, const QString& frameNonce, const QString& sender,
                           const QString& sequence, const QString& data) {
        return evp::deviceProof({"frame", "control", viewerId, hostId, frameAttempt, frameNonce,
                                 sender, "signal", sequence, data});
    };
    const auto proof = frame(attempt, nonce, "host", "2", original);
    const auto signature = host.sign(proof);
    QVERIFY(evp::DeviceIdentity::verify(host.publicKey(), proof, signature));
    QVERIFY(!evp::DeviceIdentity::verify(viewer.publicKey(), proof, signature));
    const auto verifyFrame = [&](const QByteArray& bytes) {
        return evp::DeviceIdentity::verify(host.publicKey(), bytes, signature);
    };
    QVERIFY(!verifyFrame(frame(token(), nonce, "host", "2", original)));
    QVERIFY(!verifyFrame(frame(attempt, token(), "host", "2", original)));
    QVERIFY(!verifyFrame(frame(attempt, nonce, "viewer", "2", original)));
    QVERIFY(!verifyFrame(frame(attempt, nonce, "host", "3", original)));
    QVERIFY(!verifyFrame(frame(attempt, nonce, "host", "2", payload(evp::deviceRandom()))));
}

void DeviceSecurityProbe::pausesOnUnwritableSettingsAcrossRestarts() {
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const auto path = profile.filePath("devices.dat");
    QString id;
    {
        evp::DeviceStore initial(path);
        QVERIFY(initial.open());
        id = evp::deviceId(initial.identity.publicKey());
        initial.preferences.allowIncoming = true;
        QVERIFY(initial.save());
    }
    auto heldFile = lockAgainstReplacement(path);
    QVERIFY(heldFile);
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        const device_probe::Ui ui(window);
        QCOMPARE(ui.id(), id);
        QVERIFY(!ui.online());
        QVERIFY(!window.findChild<QCheckBox*>("allowTrustedControl")->isChecked());
        auto* settingsMessage = window.findChild<QLabel*>("settingsMessage");
        QVERIFY(settingsMessage);
        QVERIFY(ui.message().contains(QStringLiteral("尚未保存")) ||
                settingsMessage->text().contains(QStringLiteral("尚未保存")));
        QVERIFY(QFile::exists(path + ".hold"));
    }
    heldFile.reset();
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        const device_probe::Ui ui(window);
        QVERIFY(!window.findChild<QCheckBox*>("allowTrustedControl")->isChecked());
        QVERIFY(!ui.online());
        window.findChild<QCheckBox*>("deviceShareAudio")->setChecked(false);
        QVERIFY(!QFile::exists(path + ".hold"));
    }
    evp::DeviceStore recovered(path);
    QVERIFY(recovered.open());
    QCOMPARE(evp::deviceId(recovered.identity.publicKey()), id);
    QVERIFY(!recovered.preferences.allowIncoming);
    QVERIFY(!recovered.preferences.audio);
}

void DeviceSecurityProbe::persistsLocalDeviceNamesAndOrder() {
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    evp::DeviceIdentity owner, peer;
    QVERIFY(owner.load({}));
    QMap<QString, QByteArray> peerKeys;
    for (int index = 0; index < 4; ++index) {
        QVERIFY(peer.load({}));
        const auto key = peer.publicKey();
        const auto id = evp::deviceId(key);
        QVERIFY(!id.isEmpty());
        peerKeys.insert(id, key);
    }
    QCOMPARE(peerKeys.size(), 4);
    const auto keys = peerKeys.keys();
    const auto first = keys[3];
    const auto second = keys[2];
    const auto third = keys[1];
    const auto fourth = keys[0];
    const QStringList legacyOrder{first, second, third, fourth};
    const QString originalName = "Original device";
    const QString alias = "Studio Laptop";
    QJsonArray peers;
    for (const auto& id : legacyOrder)
        peers.append(QJsonObject{{"publicKey", evp::encodeDeviceBytes(peerKeys.value(id))}, {"name", originalName}});
    const auto path = profile.filePath("v1.dat");
    QVERIFY(writeLegacyProfile(path, owner, {{"version", 1}, {"peers", peers}, {"url", "ws://127.0.0.1/"}}));

    {
        evp::DeviceStore legacy(path);
        QVERIFY(legacy.open());
        QCOMPARE(evp::orderedDeviceIds(legacy.peers), legacyOrder);
        for (const auto& id : legacyOrder)
            QVERIFY(!legacy.peers.value(id).pinned);
    }

    const QStringList pinInsertionOrder{fourth, second, third, first};
    const QStringList pinnedOrder{second, fourth, third, first};
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        const device_probe::Ui ui(window);
        QCOMPARE(ui.id(), evp::deviceId(owner.publicKey()));
        QVERIFY(!ui.online());
        auto* list = window.findChild<QListWidget*>("trustedDeviceList");
        QVERIFY(list);
        QCOMPARE(trustedDeviceIds(*list), legacyOrder);
        window.devices().renameRequested(third, QStringLiteral("  Studio\t  Laptop "));
        window.devices().moveRequested(third, evp::DeviceMove::Up);
        QCOMPARE(trustedDeviceIds(*list), (QStringList{first, third, second, fourth}));
        window.devices().moveRequested(first, evp::DeviceMove::Down);
        QCOMPARE(trustedDeviceIds(*list), (QStringList{third, first, second, fourth}));
        window.devices().pinRequested(second, true);
        QCOMPARE(trustedDeviceIds(*list), (QStringList{second, third, first, fourth}));
        window.devices().pinRequested(fourth, true);
        QCOMPARE(trustedDeviceIds(*list), pinInsertionOrder);

        {
            const auto heldFile = lockAgainstReplacement(path);
            QVERIFY(heldFile);
            const auto message = ui.message();
            window.devices().pinRequested(second, true);
            window.devices().pinRequested(first, false);
            window.devices().moveRequested(second, evp::DeviceMove::Down);
            window.devices().moveRequested(third, evp::DeviceMove::Up);
            QCOMPARE(trustedDeviceIds(*list), pinInsertionOrder);
            QCOMPARE(ui.message(), message);
        }

        window.devices().moveRequested(second, evp::DeviceMove::Up);
        QCOMPARE(trustedDeviceIds(*list), pinnedOrder);
    }
    {
        evp::DeviceStore reopened(path);
        QVERIFY(reopened.open());
        QCOMPARE(evp::orderedDeviceIds(reopened.peers), pinnedOrder);
        QVERIFY(reopened.peers.value(fourth).pinned);
        QVERIFY(reopened.peers.value(second).pinned);
        QVERIFY(!reopened.peers.value(third).pinned);
        QVERIFY(!reopened.peers.value(first).pinned);
    }

    const QStringList unpinnedOrder{fourth, second, third, first};
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        auto* list = window.findChild<QListWidget*>("trustedDeviceList");
        QVERIFY(list);
        QCOMPARE(trustedDeviceIds(*list), pinnedOrder);
        QCOMPARE(list->item(2)->text(), QStringLiteral("%1   ·   离线").arg(alias));
        window.devices().pinRequested(second, false);
        QCOMPARE(trustedDeviceIds(*list), unpinnedOrder);
    }

    evp::DeviceStore reopened(path);
    QVERIFY(reopened.open());
    QCOMPARE(evp::orderedDeviceIds(reopened.peers), unpinnedOrder);
    QVERIFY(reopened.peers.value(fourth).pinned);
    QVERIFY(!reopened.peers.value(second).pinned);
    QCOMPARE(reopened.peers.value(third).name, originalName);
    QCOMPARE(reopened.peers.value(third).displayName(), alias);
    for (const auto& id : legacyOrder)
        QCOMPARE(reopened.peers.value(id).publicKey, peerKeys.value(id));
}

void DeviceSecurityProbe::rejectsUnwritableDeviceMetadata_data() {
    QTest::addColumn<bool>("holdAtStartup");
    QTest::newRow("write-failure") << false;
    QTest::newRow("existing-storage-hold") << true;
}

void DeviceSecurityProbe::rejectsUnwritableDeviceMetadata() {
    QFETCH(bool, holdAtStartup);
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const auto path = profile.filePath("devices.dat");
    const QString originalName = "Original device";
    QStringList order;
    {
        evp::DeviceStore seed(path);
        QVERIFY(seed.open());
        seed.url = QUrl("ws://127.0.0.1/");
        seed.preferences.allowIncoming = true;
        evp::DeviceIdentity peer;
        for (int index = 0; index < 4; ++index) {
            QVERIFY(peer.load({}));
            const auto key = peer.publicKey();
            const auto id = evp::deviceId(key);
            QVERIFY(!id.isEmpty());
            seed.peers.insert(id, {key, originalName});
        }
        order = evp::orderedDeviceIds(seed.peers);
        QCOMPARE(order.size(), 4);
        seed.peers[order.at(0)].pinned = true;
        seed.peers[order.at(1)].pinned = true;
        QVERIFY(seed.save());
        if (holdAtStartup)
            QVERIFY(seed.markStorageHold());
    }
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        auto* list = window.findChild<QListWidget*>("trustedDeviceList");
        auto* incoming = window.findChild<QCheckBox*>("allowTrustedControl");
        auto* audio = window.findChild<QCheckBox*>("deviceShareAudio");
        QVERIFY(list && incoming && audio);
        QCOMPARE(trustedDeviceIds(*list), order);
        const auto firstRow = list->item(0)->text();
        auto heldFile = lockAgainstReplacement(path);
        QVERIFY(heldFile);
        if (holdAtStartup)
            heldFile.reset();

        window.devices().renameRequested(order.first(), QStringLiteral("Unsaved alias"));
        QCOMPARE(list->item(0)->text(), firstRow);
        window.devices().moveRequested(order.last(), evp::DeviceMove::Up);
        QCOMPARE(trustedDeviceIds(*list), order);
        window.devices().pinRequested(order.last(), true);
        QCOMPARE(trustedDeviceIds(*list), order);
        window.devices().pinRequested(order.first(), false);
        QCOMPARE(trustedDeviceIds(*list), order);
        QCOMPARE(incoming->isChecked(), !holdAtStartup);
        QCOMPARE(QFile::exists(path + ".hold"), holdAtStartup);

        if (!holdAtStartup) {
            const device_probe::Ui ui(window);
            QVERIFY(!ui.message().isEmpty());
            heldFile.reset();
            audio->setChecked(false);
        }
    }
    evp::DeviceStore reopened(path);
    QVERIFY(reopened.open());
    QCOMPARE(reopened.hasStorageHold(), holdAtStartup);
    QVERIFY(reopened.preferences.allowIncoming);
    QCOMPARE(reopened.preferences.audio, holdAtStartup);
    QCOMPARE(evp::orderedDeviceIds(reopened.peers), order);
    QCOMPARE(reopened.peers.value(order.first()).displayName(), originalName);
    QVERIFY(reopened.peers.value(order.at(0)).pinned);
    QVERIFY(reopened.peers.value(order.at(1)).pinned);
    QVERIFY(!reopened.peers.value(order.at(2)).pinned);
    QVERIFY(!reopened.peers.value(order.at(3)).pinned);
}

void DeviceSecurityProbe::lockedProfileRejectsFallbackJoin() {
    QTemporaryDir profile;
    LoopbackBroker original, fallback;
    QVERIFY(profile.isValid() && original.listen() && fallback.listen());
    const auto path = profile.filePath("devices.dat");
    evp::DeviceStore owner(path);
    QVERIFY(owner.open());
    owner.url = original.url();
    owner.deploymentKey = "synthetic-owner-key";
    QVERIFY(owner.save());

    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    evp::DesktopController controller(window, {}, path, fallback.url());
    window.show();
    const device_probe::Ui ui(window);
    ui.button("navigation")->click();
    window.findChild<QTabBar*>("assistanceTabs")->setCurrentIndex(0);
    QSignalSpy joins(&window, &HomeWindow::joinRequested);
    ui.field("invitation")->setText("synthetic-private-invitation");
    ui.button("join")->click();
    QTest::qWait(150);
    QCOMPARE(fallback.connections, 0);
    QCOMPARE(original.connections, 0);
    QCOMPARE(joins.count(), 0);
    ui.button("settingsNavigation")->click();
    for (const auto* name : {"settingsServerUrl", "settingsDeploymentKey"}) {
        auto* field = ui.field(name);
        QVERIFY(field->isVisible() && !field->isEnabled() && field->text().isEmpty());
    }
    QVERIFY(!ui.button("saveSettings")->isEnabled());
}

void DeviceSecurityProbe::retriesFailedForgetWithoutEditing() {
    QTemporaryDir profile;
    LoopbackBroker original;
    QVERIFY(profile.isValid() && original.listen());
    const auto path = profile.filePath("devices.dat");
    {
        HomeWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        evp::DesktopController controller(window, {}, path, original.url());
        window.show();
        const device_probe::Ui ui(window);
        const QString key = "synthetic-key-to-forget";
        QVERIFY(ui.saveSettings(original.url(), key));
        QTRY_COMPARE(original.frames.size(), 1);
        QCOMPARE(original.frames.first().value("type").toString(), QString("device-hello"));
        QCOMPARE(original.frames.first().value("key").toString(), key);
        QCOMPARE(original.closed, 0);

        auto* forget = window.findChild<QCheckBox*>("forgetDeploymentKey");
        auto* message = window.findChild<QLabel*>("settingsMessage");
        auto* save = ui.button("saveSettings");
        QVERIFY(forget && forget->isVisible() && message && save);
        forget->click();
        auto held = lockAgainstReplacement(path);
        QVERIFY(held);
        save->click();
        QVERIFY(forget->isChecked());
        QVERIFY(message->isVisible() && message->text().contains(QStringLiteral("尚未保存")));
        QTRY_COMPARE(original.closed, 1);
        held.reset();
        QVERIFY(save->isEnabled());
        save->click();
        QVERIFY(!forget->isChecked() && !QFile::exists(path + ".hold"));
        QTest::qWait(150);
        QCOMPARE(original.connections, 1);
        QCOMPARE(original.frames.size(), 1);
    }
    {
        evp::DeviceStore restarted(path);
        QVERIFY(restarted.open());
        QCOMPARE(restarted.url, original.url());
        QVERIFY(restarted.deploymentKey.isEmpty() && !restarted.hasStorageHold());
    }
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    evp::DesktopController controller(window, {}, path);
    window.show();
    const device_probe::Ui ui(window);
    ui.button("settingsNavigation")->click();
    QCOMPARE(QUrl(ui.field("settingsServerUrl")->text()), original.url());
    QVERIFY(!window.findChild<QCheckBox*>("forgetDeploymentKey")->isVisible());
    QTest::qWait(150);
    QCOMPARE(original.connections, 1);
}

void DeviceSecurityProbe::committedAddressSurvivesHeldMarker() {
    QTemporaryDir profile;
    LoopbackBroker original, changed;
    QVERIFY(profile.isValid() && original.listen() && changed.listen());
    const auto path = profile.filePath("devices.dat");
    const QString newKey = "synthetic-held-key";
    {
        evp::DeviceStore seed(path);
        QVERIFY(seed.open());
        seed.url = original.url();
        seed.deploymentKey = "synthetic-previous-key";
        QVERIFY(seed.save());
        QVERIFY(seed.markStorageHold());
    }
    const auto marker = lockAgainstReplacement(path + ".hold");
    QVERIFY(marker);
    for (int launch = 0; launch < 2; ++launch) {
        {
            HomeWindow window;
            window.setAttribute(Qt::WA_DontShowOnScreen);
            evp::DesktopController controller(window, {}, path, original.url());
            window.show();
            const device_probe::Ui ui(window);
            ui.button("settingsNavigation")->click();
            if (launch == 0) {
                QCOMPARE(QUrl(ui.field("settingsServerUrl")->text()), original.url());
                QVERIFY(ui.saveSettings(changed.url(), newKey));
                auto* message = window.findChild<QLabel*>("settingsMessage");
                QVERIFY(message && message->isVisible());
                QVERIFY(message->text().contains(QStringLiteral("已保存")));
                QVERIFY(message->text().contains(QStringLiteral("标记仍无法清除")));
                ui.button("deviceNavigation")->click();
                ui.button("settingsNavigation")->click();
            }
            QCOMPARE(QUrl(ui.field("settingsServerUrl")->text()), changed.url());
            QVERIFY(ui.field("settingsDeploymentKey")->text().isEmpty());
            QVERIFY(!ui.online());
            ui.button("navigation")->click();
            window.findChild<QTabBar*>("assistanceTabs")->setCurrentIndex(0);
            const auto invite = QString("synthetic-held-invitation-%1").arg(launch);
            ui.field("invitation")->setText(invite);
            ui.button("join")->click();
            const QJsonObject expected{{"type", "join"}, {"invite", invite}};
            QTRY_VERIFY(changed.frames.contains(expected));
            QTest::qWait(150);
            QCOMPARE(changed.connections, launch + 1);
            QCOMPARE(original.connections, 0);
        }
        evp::DeviceStore reopened(path);
        QVERIFY(reopened.open() && reopened.hasStorageHold());
        QCOMPARE(reopened.url, changed.url());
        QCOMPARE(reopened.deploymentKey, newKey);
    }
}

void DeviceSecurityProbe::appearanceFollowsAndPersistsTheme() {
    using namespace native_ui;
    applyTheme(Theme::System);
    const auto systemScheme = QApplication::styleHints()->colorScheme();
    const auto restore = qScopeGuard([] {
        saveTheme(Theme::System);
        applyTheme(Theme::Light);
    });
    QCOMPARE(savedTheme(), Theme::System);
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    window.showSettingsPage();
    auto* appearance = window.findChild<QPushButton*>("settingsAppearanceNavigation");
    auto* system = window.findChild<QRadioButton*>("themeSystem");
    auto* light = window.findChild<QRadioButton*>("themeLight");
    auto* dark = window.findChild<QRadioButton*>("themeDark");
    QVERIFY(appearance && system && light && dark);
    appearance->click();
    QVERIFY(appearance->isChecked() && system->isVisible() && system->isChecked());
    QVERIFY(light->isVisible() && dark->isVisible());
    QCOMPARE(system->y(), light->y());
    QCOMPARE(light->y(), dark->y());
    QVERIFY(system->x() < light->x() && light->x() < dark->x());
    auto* themeTitle = window.findChild<QLabel*>("themeTitle");
    auto* modeTitle = window.findChild<QLabel*>("themeModeTitle");
    QVERIFY(themeTitle && modeTitle);
    QCOMPARE(themeTitle->text(), QStringLiteral("主题"));
    QCOMPARE(modeTitle->text(), QStringLiteral("模式"));
    QVERIFY(modeTitle->geometry().right() < system->geometry().left());
    QVERIFY(themeTitle->mapTo(&window, themeTitle->rect().bottomLeft()).y() <
            modeTitle->mapTo(&window, modeTitle->rect().topLeft()).y());
    QVERIFY(!system->icon().isNull() && !light->icon().isNull() && !dark->icon().isNull());
    for (auto* option : {system, light, dark}) {
        QCOMPARE(option->cursor().shape(), Qt::PointingHandCursor);
        option->setEnabled(false);
        QCOMPARE(option->cursor().shape(), Qt::ArrowCursor);
        option->setEnabled(true);
        QCOMPARE(option->cursor().shape(), Qt::PointingHandCursor);
    }

    // Click each part of a theme choice through the actual widget hit test.
    const int centerY = system->height() / 2;
    const int indicatorWidth = system->style()->pixelMetric(QStyle::PM_ExclusiveIndicatorWidth, nullptr, system);
    const int textWidth = system->fontMetrics().horizontalAdvance(system->text());
    QSignalSpy systemClicks(system, &QRadioButton::clicked);
    for (const int x : {indicatorWidth / 2, indicatorWidth + system->iconSize().width() / 2,
                        system->width() - textWidth / 2}) {
        QTest::mouseClick(system, Qt::LeftButton, Qt::NoModifier, QPoint(x, centerY));
        QCOMPARE(systemClicks.count(), 1);
        systemClicks.clear();
    }

    // Simulate a platform scheme notification while the setting is still Follow system.
    QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    QTRY_COMPARE(system->palette().color(QPalette::WindowText), QColor("#e6e8ec"));
    QVERIFY(system->isChecked());
    QCOMPARE(savedTheme(), Theme::System);
    light->click();
    QVERIFY(light->isChecked());
    QCOMPARE(savedTheme(), Theme::Light);
    QCOMPARE(QApplication::styleHints()->colorScheme(), Qt::ColorScheme::Light);

    dark->click();
    QVERIFY(dark->isChecked());
    QCOMPARE(savedTheme(), Theme::Dark);
    QCOMPARE(QApplication::styleHints()->colorScheme(), Qt::ColorScheme::Dark);
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty())
        QVERIFY(window.grab().save(QDir(preview).filePath("appearance-dark.png")));
    {
        applyTheme(Theme::System);
        applyTheme(savedTheme());
        HomeWindow reopened;
        QVERIFY(reopened.findChild<QRadioButton*>("themeDark")->isChecked());
        QCOMPARE(QApplication::styleHints()->colorScheme(), Qt::ColorScheme::Dark);
    }

    window.showDevicePage();
    auto& devices = window.devices();
    devices.setIdentity("UI-PREVIEW");
    devices.setPreferences({}, true);
    devices.setRegistration(true, QStringLiteral("本机在线"));
    auto* scroll = window.findChild<QScrollArea*>("homeScroll");
    auto* permissions = window.findChild<QCheckBox*>("allowTrustedControl");
    QVERIFY(scroll && permissions);
    const auto scrollToPermissions = [&] {
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QTRY_VERIFY(permissions->height() >= permissions->sizeHint().height());
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    };
    scrollToPermissions();
    if (!preview.isEmpty())
        QVERIFY(window.grab().save(QDir(preview).filePath("devices-dark.png")));
    const auto verifyPermissions = [&](bool darkMode) {
        for (const auto* name : {"allowTrustedControl", "deviceShareAudio", "deviceRelayOnly"}) {
            auto* check = window.findChild<QCheckBox*>(name);
            QVERIFY(check && check->isVisible() && check->isEnabled());
            auto* card = check->parentWidget();
            QCOMPARE(card->objectName(), QStringLiteral("card"));
            const auto cardImage = card->grab();
            const auto cardRatio = cardImage.devicePixelRatio();
            QCOMPARE(cardImage.toImage().pixelColor(qRound(8 * cardRatio), qRound(check->geometry().center().y() * cardRatio)),
                     QColor(darkMode ? "#25272b" : "#ffffff"));
            const auto grab = check->grab();
            const auto image = grab.toImage();
            const auto labelStart = qRound(24 * grab.devicePixelRatio());
            bool hasContrastingText = false;
            for (int y = 0; y < image.height() && !hasContrastingText; ++y) {
                for (int x = labelStart; x < image.width(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    const auto luma = pixel.lightness();
                    if (pixel.alpha() == 255 && (darkMode ? luma > 160 && luma < 245 : luma < 80)) {
                        hasContrastingText = true;
                        break;
                    }
                }
            }
            const auto description = QString("%1 in %2 mode").arg(QString::fromLatin1(name),
                                                                  darkMode ? "dark" : "light");
            QVERIFY2(hasContrastingText, qPrintable(description));
        }
    };
    verifyPermissions(true);

    window.showSettingsPage();
    appearance->click();
    light->click();
    QTRY_VERIFY(light->palette().color(QPalette::WindowText).lightness() < 80);
    if (!preview.isEmpty())
        QVERIFY(window.grab().save(QDir(preview).filePath("appearance-light.png")));
    window.showDevicePage();
    scrollToPermissions();
    verifyPermissions(false);
    if (!preview.isEmpty())
        QVERIFY(window.grab().save(QDir(preview).filePath("devices-light.png")));
    window.showSettingsPage();
    appearance->click();
    system->click();
    QVERIFY(system->isChecked());
    QCOMPARE(savedTheme(), Theme::System);
    QCOMPARE(QApplication::styleHints()->colorScheme(), systemScheme);
}

void DeviceSecurityProbe::appearanceCustomizesActivePaletteAndFont() {
    using namespace native_ui;
    applyTheme(Theme::Light);
    const auto restore = qScopeGuard([] {
        applyTheme(Theme::Dark);
        resetAppearance();
        applyTheme(Theme::Light);
        resetAppearance();
        saveTheme(Theme::System);
        applyTheme(Theme::Light);
    });
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    window.showSettingsPage();
    window.findChild<QPushButton*>("settingsAppearanceNavigation")->click();
    auto* accent = window.findChild<QPushButton*>("themeAccent");
    auto* background = window.findChild<QPushButton*>("themeBackground");
    auto* foreground = window.findChild<QPushButton*>("themeForeground");
    auto* title = window.findChild<QLabel*>("themeTitle");
    auto* font = window.findChild<QComboBox*>("themeFont");
    auto* reset = window.findChild<QPushButton*>("resetAppearance");
    auto* dark = window.findChild<QRadioButton*>("themeDark");
    auto* light = window.findChild<QRadioButton*>("themeLight");
    QVERIFY(accent && background && foreground && title && font && reset && dark && light);

    const auto chooseColor = [&](QPushButton* button, const QColor& color) {
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QColorDialog*>();
            Q_ASSERT(dialog);
            dialog->setCurrentColor(color);
            dialog->accept();
        });
        button->click();
        QCOMPARE(button->text(), color.name().toUpper());
    };
    chooseColor(background, QColor("#101010"));
    QVERIFY(title->palette().color(QPalette::WindowText).lightness() > 180);
    QVERIFY(!window.centralWidget()->styleSheet().contains("background: #dbe8ff"));
    chooseColor(accent, QColor("#3a7543"));
    chooseColor(background, QColor("#e5eee8"));
    chooseColor(foreground, QColor("#2a3a2d"));
    QVERIFY(window.centralWidget()->styleSheet().contains("background: #e5eee8"));
    QCOMPARE(title->palette().color(QPalette::WindowText), QColor("#2a3a2d"));
    QVERIFY(window.centralWidget()->styleSheet().contains("background: #3a7543"));

    window.findChild<QPushButton*>("settingsConnectionNavigation")->click();
    QApplication::setActiveWindow(&window);
    auto* save = window.findChild<QPushButton*>("saveSettings");
    QVERIFY(save && save->isVisible() && save->isEnabled());
    QVERIFY(!save->hasFocus());
    const auto unfocusedSave = save->grab().toImage();
    save->setFocus(Qt::TabFocusReason);
    QVERIFY(save->hasFocus());
    QVERIFY2(save->grab().toImage() != unfocusedSave, "A custom accent must keep the primary button's focus visible.");
    window.findChild<QPushButton*>("settingsAppearanceNavigation")->click();

    const auto defaultFont = title->font();
    const auto family = QStringLiteral("Consolas");
    const int fontIndex = font->findData(family);
    QVERIFY(fontIndex > 0);
    font->setCurrentIndex(fontIndex);
    QTRY_COMPARE(title->font().family(), family);
    QCOMPARE(title->font().pixelSize(), defaultFont.pixelSize());
    QCOMPARE(title->font().weight(), defaultFont.weight());

    dark->click();
    QCOMPARE(appearanceColor(ThemeColor::Background), QColor("#191a1c"));
    chooseColor(background, QColor("#302538"));
    QCOMPARE(font->currentData().toString(), family);
    light->click();
    QCOMPARE(background->text(), QStringLiteral("#E5EEE8"));
    QCOMPARE(foreground->text(), QStringLiteral("#2A3A2D"));
    auto* reopened = new HomeWindow(&window);
    QCOMPARE(reopened->findChild<QPushButton*>("themeAccent")->text(), QStringLiteral("#3A7543"));
    QCOMPARE(reopened->findChild<QComboBox*>("themeFont")->currentData().toString(), family);
    delete reopened;

    reset->click();
    QCOMPARE(accent->text(), QStringLiteral("#2463DA"));
    QCOMPARE(background->text(), QStringLiteral("#F7F9FD"));
    QCOMPARE(foreground->text(), QStringLiteral("#182844"));
    QTRY_COMPARE(title->font().family(), defaultFont.family());
    QVERIFY(font->currentData().toString().isEmpty());
    dark->click();
    QCOMPARE(background->text(), QStringLiteral("#302538"));
}

void DeviceSecurityProbe::settingsEditorDiscardsDrafts() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    const device_probe::Ui ui(window);
    for (const auto* name : {"settingsNavigation", "settingsBack", "settingsServerUrl", "settingsDeploymentKey",
                             "saveSettings", "settingsMessage", "forgetDeploymentKey"})
        QCOMPARE(window.findChildren<QWidget*>(name).size(), 1);
    for (const auto* name : {"hostKey", "hostSignalingUrl", "signalingUrl", "deviceDeploymentKey",
                             "deviceServerUrl", "configureDevices"})
        QVERIFY2(window.findChildren<QWidget*>(name).isEmpty(), name);

    const QUrl committed("ws://127.0.0.1:8881/");
    window.setConnectionSettings(committed, true);
    auto* address = ui.field("settingsServerUrl");
    auto* key = ui.field("settingsDeploymentKey");
    auto* forget = window.findChild<QCheckBox*>("forgetDeploymentKey");
    QVERIFY(address && key && forget);
    QSignalSpy saves(&window, &HomeWindow::settingsRequested);
    auto* back = ui.button("settingsBack");
    auto* tabs = window.findChild<QTabBar*>("assistanceTabs");
    QVERIFY(back && !back->icon().isNull() && tabs);
    for (const auto* destination : {"navigation", "deviceNavigation"}) {
        ui.button(destination)->click();
        if (tabs->isVisible())
            tabs->setCurrentIndex(1);
        ui.button("settingsNavigation")->click();
        QVERIFY(address->isVisible() && key->isVisible());
        QVERIFY(back->isVisible() && !ui.button("navigation")->isVisible() &&
                !ui.button("deviceNavigation")->isVisible());
        QCOMPARE(QUrl(address->text()), committed);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        QVERIFY(key->text().isEmpty() && !forget->isChecked());
        key->setText("draft-for-previous-address");
        address->setText("ws://127.0.0.1:8882/");
        QVERIFY(key->text().isEmpty());
        key->setText("draft-only");
        back->click();
        QVERIFY(!key->isVisible() && key->text().isEmpty());
        QVERIFY(!back->isVisible() && ui.button(destination)->isVisible());
        QCOMPARE(tabs->isVisible(), qstrcmp(destination, "navigation") == 0);
        if (tabs->isVisible())
            QCOMPARE(tabs->currentIndex(), 1);
    }
    QCOMPARE(saves.count(), 0);
    ui.button("settingsNavigation")->click();
    QCOMPARE(QUrl(address->text()), committed);
    QVERIFY(key->text().isEmpty());
    auto* about = ui.button("settingsAboutNavigation");
    auto* connection = ui.button("settingsConnectionNavigation");
    auto* checkForUpdates = ui.button("checkForUpdates");
    QVERIFY(about && connection && checkForUpdates && !checkForUpdates->isVisible());
    about->click();
    QVERIFY(checkForUpdates->isVisible() && !address->isVisible());
    QSignalSpy checks(&window, &HomeWindow::checkForUpdatesRequested);
    checkForUpdates->click();
    QCOMPARE(checks.count(), 1);
    connection->click();
    QVERIFY(address->isVisible() && !checkForUpdates->isVisible());
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty()) {
        QCoreApplication::processEvents();
        QTest::qWait(100);
        QVERIFY(window.grab().save(QDir(preview).filePath("settings.png")));
    }
}

void DeviceSecurityProbe::guestJoinsWithAddressOnly() {
    QTemporaryDir profile;
    LoopbackBroker fallback, broker;
    QVERIFY(profile.isValid() && fallback.listen() && broker.listen());
    const auto path = profile.filePath("guest.dat");
    {
        HomeWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        evp::DesktopController controller(window, {}, path, fallback.url());
        window.show();
        const device_probe::Ui ui(window);
        ui.button("settingsNavigation")->click();
        QCOMPARE(QUrl(ui.field("settingsServerUrl")->text()), fallback.url());
        QVERIFY(ui.saveSettings(broker.url()));
        QTest::qWait(150);
        QCOMPARE(broker.connections, 0);
        QCOMPARE(fallback.connections, 0);
    }
    QSettings().setValue("signaling/url", fallback.url().toString());
    const auto cleanup = qScopeGuard([] { QSettings().remove("signaling/url"); });
    {
        HomeWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        evp::DesktopController controller(window, {}, path, fallback.url());
        window.show();
        const device_probe::Ui ui(window);
        ui.button("settingsNavigation")->click();
        QCOMPARE(QUrl(ui.field("settingsServerUrl")->text()), broker.url());
        QVERIFY(ui.field("settingsDeploymentKey")->text().isEmpty());
        ui.button("deviceNavigation")->click();
        QVERIFY(!ui.online() && !ui.button("generatePairingCode")->isEnabled());
        QTest::qWait(150);
        QCOMPARE(broker.connections, 0);

        ui.button("navigation")->click();
        auto* tabs = window.findChild<QTabBar*>("assistanceTabs");
        tabs->setCurrentIndex(1);
        QSignalSpy hostRequests(&window, &HomeWindow::hostRequested);
        ui.button("hostCreate")->click();
        QCOMPARE(hostRequests.count(), 0);
        QVERIFY(ui.field("settingsServerUrl")->isVisible());
        QVERIFY(!window.findChild<QLabel*>("settingsMessage")->text().isEmpty());
        ui.button("navigation")->click();
        tabs->setCurrentIndex(0);
        const QString invite = "probe-guest-invitation";
        ui.field("invitation")->setText(invite);
        ui.button("join")->click();
        QTRY_COMPARE(broker.frames.size(), 1);
        const QJsonObject expected{{"type", "join"}, {"invite", invite}};
        QCOMPARE(broker.frames.first(), expected);
        ui.button("settingsNavigation")->click();
        auto* save = ui.button("saveSettings");
        QVERIFY(save->isVisible() && !save->isEnabled());
        QSignalSpy saves(&window, &HomeWindow::settingsRequested);
        save->click();
        QCOMPARE(saves.count(), 0);
        QCOMPARE(broker.connections, 1);
        QCOMPARE(fallback.connections, 0);
    }
}

void DeviceSecurityProbe::storedKeyIsScopedToAddress() {
    QTemporaryDir profile;
    LoopbackBroker original, changed;
    QVERIFY(profile.isValid() && original.listen() && changed.listen());
    const auto path = profile.filePath("scoped.dat");
    const QString key = "synthetic-original", rotated = "synthetic-rotation";
    {
        HomeWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        evp::DesktopController controller(window, {}, path, original.url());
        window.show();
        const device_probe::Ui ui(window);
        QVERIFY(ui.saveSettings(original.url(), key));
        QTRY_COMPARE(original.frames.size(), 1);
        QCOMPARE(original.frames.first().value("type").toString(), QString("device-hello"));
        QCOMPARE(original.frames.first().value("key").toString(), key);
        auto equivalent = original.url();
        equivalent.setPath({});
        QVERIFY(ui.saveSettings(equivalent));
    }
    {
        evp::DeviceStore store(path);
        QVERIFY(store.open());
        QCOMPARE(store.url, original.url());
        QCOMPARE(store.deploymentKey, key);
    }
    {
        HomeWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        evp::DesktopController controller(window, {}, path, changed.url());
        window.show();
        const device_probe::Ui ui(window);
        ui.button("settingsNavigation")->click();
        QCOMPARE(QUrl(ui.field("settingsServerUrl")->text()), original.url());
        QVERIFY(ui.field("settingsDeploymentKey")->text().isEmpty());
        QVERIFY(ui.saveSettings(changed.url()));
        QTest::qWait(150);
        QCOMPARE(changed.connections, 0);
    }
    {
        evp::DeviceStore store(path);
        QVERIFY(store.open());
        QCOMPARE(store.url, changed.url());
        QVERIFY(store.deploymentKey.isEmpty());
    }
    {
        HomeWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        evp::DesktopController controller(window, {}, path, original.url());
        window.show();
        const device_probe::Ui ui(window);
        QVERIFY(ui.saveSettings(changed.url(), key));
        QTRY_COMPARE(changed.frames.size(), 1);
        QVERIFY(ui.saveSettings(changed.url(), rotated));
        QTRY_COMPARE(changed.frames.size(), 2);
        QCOMPARE(changed.frames.last().value("type").toString(), QString("device-hello"));
        QCOMPARE(changed.frames.last().value("key").toString(), rotated);
        ui.button("settingsNavigation")->click();
        auto* forget = window.findChild<QCheckBox*>("forgetDeploymentKey");
        QVERIFY(forget && forget->isVisible() && forget->isEnabled());
        forget->click();
        ui.button("saveSettings")->click();
        QTRY_COMPARE(changed.closed, changed.connections);
        QVERIFY(!ui.online() && !ui.button("generatePairingCode")->isEnabled());
    }
    evp::DeviceStore store(path);
    QVERIFY(store.open());
    QCOMPARE(store.url, changed.url());
    QVERIFY(store.deploymentKey.isEmpty());
}

void DeviceSecurityProbe::readsLegacyConnectionSettings_data() {
    QTest::addColumn<bool>("savedEndpoint");
    QTest::newRow("protected-url-and-key-win") << true;
    QTest::newRow("legacy-settings-url-before-package") << false;
}

void DeviceSecurityProbe::readsLegacyConnectionSettings() {
    QFETCH(bool, savedEndpoint);
    QTemporaryDir profile;
    LoopbackBroker owned, legacy, fallback;
    QVERIFY(profile.isValid() && owned.listen() && legacy.listen() && fallback.listen());
    evp::DeviceIdentity owner, peer;
    QVERIFY(owner.load({}) && peer.load({}));
    const QString key = "synthetic-v1-key";
    const QJsonArray peers{QJsonObject{{"publicKey", evp::encodeDeviceBytes(peer.publicKey())}, {"name", "Test peer"}}};
    const QJsonObject v1{{"version", 1}, {"peers", peers}, {"url", savedEndpoint ? owned.url().toString() : QString{}},
                         {"key", savedEndpoint ? key : QString{}}, {"audio", false}, {"highRefresh", true},
                         {"allowIncoming", false}, {"relayOnly", true}};
    const auto path = profile.filePath("v1.dat");
    QVERIFY(writeLegacyProfile(path, owner, v1));
    QSettings().setValue("signaling/url", legacy.url().toString());
    const auto cleanup = qScopeGuard([] { QSettings().remove("signaling/url"); });
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    evp::DesktopController controller(window, {}, path, fallback.url());
    window.show();
    const device_probe::Ui ui(window);
    ui.button("settingsNavigation")->click();
    QCOMPARE(QUrl(ui.field("settingsServerUrl")->text()), savedEndpoint ? owned.url() : legacy.url());
    QVERIFY(ui.field("settingsDeploymentKey")->text().isEmpty());
    QCOMPARE(ui.id(), evp::deviceId(owner.publicKey()));
    QCOMPARE(ui.peer(), evp::deviceId(peer.publicKey()));
    QVERIFY(!window.findChild<QCheckBox*>("deviceShareAudio")->isChecked());
    QVERIFY(!window.findChild<QCheckBox*>("deviceHighRefresh"));
    QVERIFY(window.findChild<QCheckBox*>("deviceRelayOnly")->isChecked());
    if (savedEndpoint) {
        QTRY_COMPARE(owned.frames.size(), 1);
        QVERIFY(!ui.field("settingsDeploymentKey")->placeholderText().contains(key));
        QCOMPARE(owned.frames.first().value("type").toString(), QString("device-hello"));
        QCOMPARE(owned.frames.first().value("key").toString(), key);
        ui.button("navigation")->click();
        window.findChild<QTabBar*>("assistanceTabs")->setCurrentIndex(0);
        const QString invite = "synthetic-owned-viewer-invitation";
        ui.field("invitation")->setText(invite);
        ui.button("join")->click();
        QTRY_COMPARE(owned.frames.size(), 2);
        const QJsonObject expected{{"type", "join"}, {"invite", invite}};
        QCOMPARE(owned.frames.last(), expected);
        QCOMPARE(owned.connections, 2);
    } else {
        QTest::qWait(150);
        QCOMPARE(owned.connections, 0);
    }
    QCOMPARE(legacy.connections, 0);
    QCOMPARE(fallback.connections, 0);
}

void DeviceSecurityProbe::assistanceTabsAndScrollableDeviceList() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    auto* tabs = window.findChild<QTabBar*>("assistanceTabs");
    auto* navigation = window.findChild<QPushButton*>("navigation");
    QVERIFY(tabs && navigation && window.findChild<QPushButton*>("deviceNavigation"));
    QVERIFY(!window.findChild<QPushButton*>("hostNavigation"));
    navigation->click();
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("远程协助"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("共享本机"));
    QCOMPARE(tabs->currentIndex(), 0);
    window.setConnectionSettings(QUrl("ws://127.0.0.1/"), false);
    QSignalSpy joinRequest(&window, &HomeWindow::joinRequested);
    window.findChild<QLineEdit*>("invitation")->setText("EVP1.preview.private");
    window.findChild<QPushButton*>("join")->click();
    QCOMPARE(joinRequest.count(), 0);
    QVERIFY(window.findChild<QLabel*>("bannerMessage")->text().contains(QStringLiteral("我的设备")));
    window.findChild<QLineEdit*>("invitation")->clear();
    tabs->setFocus();
    QTest::keyClick(tabs, Qt::Key_Right);
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(window.findChild<QPushButton*>("hostCreate")->isVisible());
    const QList<int> minutes{5, 15, 30, 60, 120, 300, 720};
    for (const auto* name : {"invitationLifetime", "pairingLifetime"}) {
        auto* combo = window.findChild<QComboBox*>(name);
        QVERIFY(combo);
        QCOMPARE(combo->currentData().toInt(), 5);
        QCOMPARE(combo->count(), minutes.size());
        for (int index = 0; index < minutes.size(); ++index)
            QCOMPARE(combo->itemData(index).toInt(), minutes.at(index));
    }
    QVERIFY(!window.findChild<QCheckBox*>("autoAcceptInvitation")->isChecked());
    QCoreApplication::processEvents();
    if (!preview.isEmpty())
        QVERIFY(window.grab().save(QDir(preview).filePath("assistance.png")));

    window.showDevicePage();
    QVERIFY(!tabs->isVisible());
    QVERIFY(!window.findChild<QCheckBox*>("autoAcceptPairing")->isChecked());
    auto& page = window.devices();
    page.setIdentity("UI-PREVIEW");
    page.setRegistration(true, QStringLiteral("本机在线"));
    QMap<QString, evp::DevicePeer> peers;
    QSet<QString> online;
    const QStringList names{QStringLiteral("办公室电脑"), QStringLiteral("家用台式机"),
                            QStringLiteral("工作笔记本"), QStringLiteral("客厅电脑")};
    for (int index = 0; index < 24; ++index) {
        const auto id = QString("preview-%1").arg(index, 2, 10, QChar('0'));
        peers.insert(id, {{}, index < names.size() ? names.at(index) : QStringLiteral("测试设备 %1").arg(index)});
        if (index % 3 != 1)
            online.insert(id);
    }
    page.setDevices(peers, online);
    auto* list = window.findChild<QListWidget*>("trustedDeviceList");
    QVERIFY(list);
    QCOMPARE(list->count(), peers.size());
    QTRY_VERIFY(list->verticalScrollBar()->maximum() > 0);
    list->setCurrentRow(1);
    QTest::keyClick(list, Qt::Key_Down);
    QCOMPARE(list->currentRow(), 2);
    const auto selected = list->currentItem()->data(Qt::UserRole).toString();
    auto* connectButton = window.findChild<QPushButton*>("connectDevice");
    QVERIFY(connectButton->isEnabled());
    QTest::qWait(120);
    if (!preview.isEmpty()) {
        QVERIFY(window.grab().save(QDir(preview).filePath("devices.png")));
        auto* pairingLifetime = window.findChild<QComboBox*>("pairingLifetime");
        pairingLifetime->setCurrentIndex(pairingLifetime->findData(120));
        auto* pageScroll = window.findChild<QScrollArea*>("homeScroll");
        auto* pairingCard = pairingLifetime->parentWidget();
        pageScroll->verticalScrollBar()->setValue(pairingCard->mapTo(pageScroll->widget(), QPoint{}).y() - 16);
        QTest::qWait(100);
        QVERIFY(window.grab().save(QDir(preview).filePath("pairing-options.png")));
    }
    auto* scroll = list->verticalScrollBar();
    scroll->setValue(scroll->maximum());
    const int position = scroll->value();
    online.remove(selected);
    page.setDevices(peers, online);
    QCoreApplication::processEvents();
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), selected);
    QCOMPARE(scroll->value(), position);
    QVERIFY(!connectButton->isEnabled());
    const auto* last = list->item(list->count() - 1);
    const auto lastId = last->data(Qt::UserRole).toString();
    connect(&page, &DevicePage::pinRequested, &page, [&](const QString& id, bool pinned) {
        peers[id].pinned = pinned;
        page.setDevices(peers, online);
    });
    const auto lastRect = list->visualItemRect(last);
    const QPoint pin(lastRect.right() - 21, lastRect.center().y());
    QTest::mouseMove(list->viewport(), pin);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, pin);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), lastId);
    QCOMPARE(list->currentRow(), 0);
    QTRY_VERIFY(list->viewport()->rect().contains(list->visualItemRect(list->item(0)).center()));
    navigation->click();
    QCOMPARE(tabs->currentIndex(), 1);
}

void DeviceSecurityProbe::trustedDeviceMenuActions() {
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const auto path = profile.filePath("devices.dat");
    {
        evp::DeviceStore seed(path);
        QVERIFY(seed.open());
        for (int i = 0; i < 3; ++i) {
            evp::DeviceIdentity peer;
            QVERIFY(peer.load({}));
            const auto key = peer.publicKey();
            seed.peers.insert(evp::deviceId(key), {key, QStringLiteral("设备 %1").arg(i + 1)});
        }
        QVERIFY(seed.save());
    }
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    evp::DesktopController controller(window, {}, path);
    window.show();
    window.showDevicePage();
    auto* list = window.findChild<QListWidget*>("trustedDeviceList");
    QVERIFY(list && list->count() == 3);
    const auto initial = trustedDeviceIds(*list);
    const auto selectedName = list->item(1)->text().section("   ·   ", 0, 0);
    const auto contextAt = [&](QPoint point) {
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, list->viewport()->mapToGlobal(point));
        QApplication::sendEvent(list->viewport(), &event);
        return qobject_cast<QMenu*>(QApplication::activePopupWidget());
    };
    const auto menuForRow = [&](int row) {
        auto* item = list->item(row);
        list->scrollToItem(item);
        return contextAt(list->visualItemRect(item).center());
    };
    const auto choose = [&](int row, const char* actionName) {
        auto* menu = menuForRow(row);
        auto* action = menu ? menu->findChild<QAction*>(actionName) : nullptr;
        if (!action || !action->isEnabled()) return false;
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
        return true;
    };
    auto* menu = menuForRow(1);
    QVERIFY(menu);
    QCOMPARE(list->currentRow(), 1);
    QStringList labels;
    for (auto* action : menu->actions())
        if (!action->isSeparator()) labels << action->text();
    QCOMPARE(labels, QStringList({QStringLiteral("重命名"), QStringLiteral("上移"), QStringLiteral("下移"),
                                  QStringLiteral("置顶"), QStringLiteral("移除")}));
    menu->close();

    QVERIFY(choose(1, "renameDeviceAction"));
    auto* rename = window.findChild<QInputDialog*>("renameDeviceDialog");
    QVERIFY(rename && rename->isVisible());
    QCOMPARE(rename->textValue(), selectedName);
    rename->setTextValue("  Desk  ");
    rename->accept();
    QVERIFY(list->currentItem()->text().startsWith("Desk   ·"));
    QVERIFY(choose(1, "renameDeviceAction"));
    rename = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
    QVERIFY(rename);
    QCOMPARE(rename->textValue(), QStringLiteral("Desk"));
    rename->setTextValue(QStringLiteral("放弃改名"));
    rename->reject();
    QVERIFY(list->currentItem()->text().startsWith("Desk   ·"));
    QVERIFY(choose(1, "moveDeviceUpAction"));
    QCOMPARE(trustedDeviceIds(*list), QStringList({initial[1], initial[0], initial[2]}));
    QCOMPARE(list->currentRow(), 0);
    menu = menuForRow(0);
    QVERIFY(menu);
    auto* up = menu->findChild<QAction*>("moveDeviceUpAction");
    auto* top = menu->findChild<QAction*>("moveDeviceTopAction");
    QVERIFY(up && top && !up->isEnabled() && top->isEnabled());
    menu->close();
    QVERIFY(choose(0, "moveDeviceDownAction"));
    QCOMPARE(trustedDeviceIds(*list), initial);
    menu = menuForRow(2);
    QVERIFY(menu);
    auto* down = menu->findChild<QAction*>("moveDeviceDownAction");
    QVERIFY(down && !down->isEnabled());
    menu->close();
    QVERIFY(choose(2, "moveDeviceTopAction"));
    QCOMPARE(trustedDeviceIds(*list), QStringList({initial[2], initial[0], initial[1]}));
    QCOMPARE(list->currentRow(), 0);
    menu = menuForRow(0);
    QVERIFY(menu && !up->isEnabled() && !down->isEnabled());
    QCOMPARE(top->text(), QStringLiteral("取消置顶"));
    menu->close();
    const auto pinPoint = [&](int row) {
        const auto rect = list->visualItemRect(list->item(row));
        return QPoint(rect.right() - 21, rect.center().y());
    };
    QSignalSpy connections(&window.devices(), &DevicePage::connectRequested);
    QTest::mouseMove(list->viewport(), pinPoint(2));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, pinPoint(2));
    QCOMPARE(trustedDeviceIds(*list), QStringList({initial[1], initial[2], initial[0]}));
    menu = menuForRow(0);
    QVERIFY(menu && down->isEnabled());
    QCOMPARE(top->text(), QStringLiteral("取消置顶"));
    menu->close();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, pinPoint(0));
    QCOMPARE(trustedDeviceIds(*list), QStringList({initial[2], initial[1], initial[0]}));
    QCOMPARE(connections.count(), 0);
    QVERIFY(choose(1, "moveDeviceDownAction"));
    QVERIFY(choose(1, "removeDeviceAction"));
    QCOMPARE(trustedDeviceIds(*list), QStringList({initial[2], initial[1]}));

    const auto empty = QPoint(list->viewport()->width() / 2, list->viewport()->height() - 1);
    QVERIFY(!list->itemAt(empty));
    QVERIFY(!contextAt(empty));

    // A mutation while the menu is open must not retarget the next row.
    menu = menuForRow(0);
    QVERIFY(menu);
    window.devices().removeRequested(initial[2]);
    QTRY_VERIFY(!QApplication::activePopupWidget());
    QCOMPARE(trustedDeviceIds(*list), QStringList{initial[1]});
    menu = menuForRow(0);
    QVERIFY(menu);
    for (const auto* name : {"moveDeviceUpAction", "moveDeviceDownAction"}) {
        auto* action = menu->findChild<QAction*>(name);
        QVERIFY(action && !action->isEnabled());
    }
    QVERIFY(top->isEnabled());
    menu->close();
}

void DeviceSecurityProbe::trustedDevicePinHover() {
    native_ui::applyTheme(native_ui::Theme::Dark);
    const auto restore = qScopeGuard([] { native_ui::applyTheme(native_ui::Theme::Light); });
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    window.showDevicePage();
    auto& page = window.devices();
    QMap<QString, evp::DevicePeer> peers;
    peers["pinned"].name = QStringLiteral("办公室电脑");
    peers["pinned"].pinned = true;
    peers["regular"].name = QStringLiteral("开发机");
    page.setDevices(peers, {"regular"});
    page.setRegistration(true, QStringLiteral("本机在线"));
    auto* list = window.findChild<QListWidget*>("trustedDeviceList");
    QVERIFY(list && list->count() == 2);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("pinned"));
    QCoreApplication::processEvents();
    auto* viewport = list->viewport();
    const auto pinCenter = [&](int row) {
        const auto rect = list->visualItemRect(list->item(row));
        return QPoint(qMin(rect.right(), viewport->rect().right()) - 21, rect.center().y());
    };
    const auto pinHasInk = [&](int row) {
        const auto area = QRect(pinCenter(row) - QPoint(9, 9), QSize(19, 19));
        const auto pixels = viewport->grab(area).toImage();
        const auto background = pixels.pixel(0, 0);
        int changed = 0;
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                if (pixels.pixel(x, y) != background) ++changed;
        return changed >= 8;
    };
    // The offscreen window has no native cursor. Deliver the viewport events
    // that Qt would generate for a move over the center of the actual row.
    const auto hoverRow = [&](int row) {
        const auto pos = list->visualItemRect(list->item(row)).intersected(viewport->rect()).center();
        const auto global = viewport->mapToGlobal(pos);
        QMouseEvent move(QEvent::MouseMove, pos, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QHoverEvent hover(QEvent::HoverMove, pos, global, QPointF(-1, -1));
        QApplication::sendEvent(viewport, &hover);
    };
    hoverRow(0);
    QTRY_VERIFY(!pinHasInk(1));
    hoverRow(1);
    QTRY_VERIFY(pinHasInk(1));
    QVERIFY(pinHasInk(0));
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty())
        QVERIFY(list->grab().save(QDir(preview).filePath("device-pins.png")));
    list->setCurrentRow(1);
    auto* connectButton = window.findChild<QPushButton*>("connectDevice");
    QVERIFY(connectButton && connectButton->isEnabled());
    QSignalSpy connections(&page, &DevicePage::connectRequested);
    list->setFixedSize(270, 165);
    peers["spare"].name = QString(80, QChar(u'机'));
    page.setDevices(peers, {"pinned", "regular", "spare"});
    QCoreApplication::processEvents();
    QCOMPARE(trustedDeviceIds(*list), QStringList({"pinned", "regular", "spare"}));
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(1)).center());
    hoverRow(2);
    const auto pin = pinCenter(2) - QPoint(14, 0);  // leftmost pixel in the pin target
    QVERIFY(viewport->rect().contains(pin));
    QVERIFY(pin.x() >= viewport->width() - 40);
    QVERIFY(pinHasInk(2));
    connect(&page, &DevicePage::pinRequested, &page, [&](const QString& id, bool value) {
        peers[id].pinned = value;
        peers[id].order = -1;
        page.setDevices(peers, {"pinned", "regular", "spare"});
    });
    QSignalSpy pins(&page, &DevicePage::pinRequested);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, pin);
    QCOMPARE(trustedDeviceIds(*list), QStringList({"spare", "pinned", "regular"}));
    // The original screen position now contains an unpinned device. Its pin
    // must be visible even before another movement event reaches the view.
    QVERIFY(pinHasInk(2));
    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, pin - QPoint(1, 0));
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, pin - QPoint(1, 0));
    QCOMPARE(pins.count(), 1);
    QCOMPARE(connections.count(), 0);
}

void DeviceSecurityProbe::cleanTabFocus() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QApplication::setActiveWindow(&window);
    auto* tabs = window.findChild<QTabBar*>("assistanceTabs");
    auto* navigation = window.findChild<QPushButton*>("navigation");
    QVERIFY(tabs && navigation);
    tabs->setFocus(Qt::MouseFocusReason);
    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(tabs->hasFocus());
    const QImage clicked = tabs->grab().toImage();
    navigation->setFocus(Qt::OtherFocusReason);
    QVERIFY(!tabs->hasFocus());
    const QImage unfocused = tabs->grab().toImage();
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty())
        QVERIFY(clicked.save(QDir(preview).filePath("clicked-tabs.png")));
    QVERIFY2(clicked == unfocused, "A mouse click drew an extra focus rectangle on the selected tab.");
    QTest::keyClick(navigation, Qt::Key_Tab);
    QVERIFY(tabs->hasFocus());
    const QImage keyboard = tabs->grab().toImage();
    QVERIFY2(keyboard != unfocused, "Keyboard navigation needs a visible focus indicator.");
    if (!preview.isEmpty())
        QVERIFY(keyboard.save(QDir(preview).filePath("keyboard-tabs.png")));
    QTest::keyClick(tabs, Qt::Key_Right);
    QCOMPARE(tabs->currentIndex(), 1);
    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(tabs->grab().toImage() == clicked);
}

void DeviceSecurityProbe::cleanSidebarFocus_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void DeviceSecurityProbe::cleanSidebarFocus() {
    QFETCH(bool, dark);
    native_ui::applyTheme(dark ? native_ui::Theme::Dark : native_ui::Theme::Light);
    const auto restore = qScopeGuard([] { native_ui::applyTheme(native_ui::Theme::Light); });
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QApplication::setActiveWindow(&window);
    window.showDevicePage();

    auto* button = window.findChild<QPushButton*>("deviceNavigation");
    auto* assistance = window.findChild<QPushButton*>("navigation");
    auto* settings = window.findChild<QPushButton*>("settingsNavigation");
    auto* back = window.findChild<QPushButton*>("settingsBack");
    QVERIFY(button && assistance && settings && back && button->isChecked());
    const auto interior = [&] { return button->grab(button->rect().adjusted(4, 4, -4, -4)).toImage(); };
    assistance->setFocus(Qt::OtherFocusReason);
    QVERIFY(!button->hasFocus());
    const auto unfocused = button->grab().toImage();
    const auto unfocusedInterior = interior();

    button->setFocus(Qt::TabFocusReason);
    QVERIFY(button->hasFocus());
    QVERIFY2(button->grab().toImage() != unfocused, "Keyboard focus needs a visible border.");
    QVERIFY2(interior() == unfocusedInterior, "Keyboard focus drew a second border inside the navigation button.");

    QTest::mouseClick(settings, Qt::LeftButton);
    QTest::mouseClick(back, Qt::LeftButton);
    QVERIFY(button->hasFocus() && button->isChecked());
    QVERIFY2(interior() == unfocusedInterior, "Returning from settings drew an inner focus border.");
}

void DeviceSecurityProbe::cleanDeviceListFocus() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.showDevicePage();
    window.devices().setRegistration(true, QStringLiteral("本机在线"));
    window.devices().setDevices({{"first", {{}, QStringLiteral("办公室电脑")}},
                                 {"second", {{}, QStringLiteral("家用台式机")}}}, {"first", "second"});
    window.show();
    QApplication::setActiveWindow(&window);

    auto* list = window.findChild<QListWidget*>("trustedDeviceList");
    auto* connectButton = window.findChild<QPushButton*>("connectDevice");
    QVERIFY(list && connectButton);
    const auto first = list->visualItemRect(list->item(0));
    QVERIFY(!first.isEmpty());
    // Crop the row border so the customized selection and focus colors can differ.
    const auto rowInterior = [&] { return list->viewport()->grab(first.adjusted(3, 3, -3, -3)).toImage(); };
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, first.center());
    QCOMPARE(list->currentRow(), 0);
    QVERIFY(list->hasFocus());
    QVERIFY(connectButton->isEnabled());
    const auto clicked = rowInterior();
    connectButton->setFocus(Qt::OtherFocusReason);
    QVERIFY(!list->hasFocus());
    QCOMPARE(list->selectedItems().size(), 1);
    const auto unfocused = rowInterior();
    QVERIFY2(clicked == unfocused, "Mouse focus added a dotted rectangle inside the selected device row.");

    QTest::keyClick(connectButton, Qt::Key_Tab, Qt::ShiftModifier);
    QVERIFY(list->hasFocus());
    QCOMPARE(list->currentRow(), 0);
    QVERIFY2(rowInterior() != unfocused, "Shift+Tab needs a visible focus cue in the selected device row.");
    QTest::keyClick(list, Qt::Key_Down);
    QCOMPARE(list->currentRow(), 1);

    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, first.center());
    QCOMPARE(list->currentRow(), 0);
    QVERIFY(list->hasFocus());
    QVERIFY2(rowInterior() == clicked, "A click must clear the keyboard cue even while the list keeps focus.");
    QTest::keyClick(list, Qt::Key_Tab);
    QVERIFY(connectButton->hasFocus());
}

void DeviceSecurityProbe::checkBoxCursorFollowsEnabledState() {
    HomeWindow window;
    const auto boxes = window.findChildren<QCheckBox*>();
    auto* invitation = window.findChild<QCheckBox*>("autoAcceptInvitation");
    QVERIFY(invitation && invitation->isEnabled());
    for (const auto enabled : {true, false, true}) {
        window.setEnabled(enabled);
        for (const auto* box : boxes)
            QCOMPARE(box->cursor().shape(), box->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    invitation->setDisabled(true);
    QCOMPARE(invitation->cursor().shape(), Qt::ArrowCursor);
    invitation->setEnabled(true);
    QCOMPARE(invitation->cursor().shape(), Qt::PointingHandCursor);
}

void DeviceSecurityProbe::cleanCheckBoxFocus() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.findChild<QTabBar*>("assistanceTabs")->setCurrentIndex(1);
    window.show();
    QApplication::setActiveWindow(&window);

    auto* check = window.findChild<QCheckBox*>("autoAcceptInvitation");
    auto* field = window.findChild<QComboBox*>("invitationLifetime");
    QVERIFY(check && check->isVisible() && check->isEnabled());
    QVERIFY(field);
    check->setFocus(Qt::TabFocusReason);
    QVERIFY(check->hasFocus());

    QTest::mouseClick(check, Qt::LeftButton, Qt::NoModifier, QPoint(6, check->height() / 2));
    QVERIFY(check->isChecked());
    QVERIFY(check->hasFocus());
    const auto clicked = check->grab().toImage();
    field->setFocus(Qt::OtherFocusReason);
    QVERIFY(!check->hasFocus());
    QVERIFY2(check->grab().toImage() == clicked, "A checkbox click displayed its keyboard focus rectangle.");

    check->setFocus(Qt::ShortcutFocusReason);
    QVERIFY(check->hasFocus());
    const auto keyboard = check->grab().toImage();
    bool solidLine = false;
    for (int y = 0; y < keyboard.height() && !solidLine; ++y) {
        int length = 0;
        for (int x = 0; x < keyboard.width(); ++x) {
            length = keyboard.pixel(x, y) != clicked.pixel(x, y) ? length + 1 : 0;
            if (length >= 12) {
                solidLine = true;
                break;
            }
        }
    }
    QVERIFY2(solidLine, "Shortcut focus needs a continuous focus line on the checkbox.");
    QTest::keyClick(check, Qt::Key_Space);
    QVERIFY(!check->isChecked());
}

void DeviceSecurityProbe::themedLifetimeSelectors_data() {
    QTest::addColumn<bool>("pairing");
    QTest::newRow("invitation") << false;
    QTest::newRow("pairing") << true;
}

void DeviceSecurityProbe::themedLifetimeSelectors() {
    QFETCH(bool, pairing);
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    auto& devices = window.devices();
    if (pairing)
        window.showDevicePage();
    else
        window.findChild<QTabBar*>("assistanceTabs")->setCurrentIndex(1);
    auto* field = window.findChild<native_ui::SelectField*>(pairing ? "pairingLifetime" : "invitationLifetime");
    QVERIFY(field);
    QCOMPARE(field->accessibleName(), pairing ? QStringLiteral("配对码有效期") : QStringLiteral("邀请码有效期"));
    if (pairing) {
        QVERIFY(!field->isEnabled());
        QVERIFY(!window.findChild<QPushButton*>("generatePairingCode")->isEnabled());
        QVERIFY(window.findChild<QLineEdit*>("localPairingCode")->isReadOnly());
        devices.setIdentity("UI-PREVIEW");
        devices.setPreferences({}, true);
        devices.setRegistration(true, QStringLiteral("本机在线"));
        auto* scroll = window.findChild<QScrollArea*>("homeScroll");
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    }
    QTest::qWait(100);
    QVERIFY(field->isEnabled());
    auto* view = field->view();
    auto* popup = view->window();
    QVERIFY(popup != &window);
    popup->setAttribute(Qt::WA_DontShowOnScreen);
    QSignalSpy changed(field, &QComboBox::currentIndexChanged);
    field->setFocus();
    QTest::keyClick(field, Qt::Key_Down, Qt::AltModifier);
    QTRY_VERIFY(popup->isVisible());
    QTRY_VERIFY(view->visualRect(field->model()->index(0, 0)).height() >= 32);
    QTest::keyClick(view, Qt::Key_Down);
    QTest::keyClick(view, Qt::Key_Escape);
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(field->currentData().toInt(), 5);
    QCOMPARE(changed.count(), 0);

    field->showPopup();
    QTRY_VERIFY(popup->isVisible());
    QTest::keyClick(view, Qt::Key_Down);
    QTest::keyClick(view, Qt::Key_Return);
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(field->currentData().toInt(), 15);
    QCOMPARE(changed.count(), 1);
    QTest::mouseClick(field, Qt::LeftButton);
    QTRY_VERIFY(popup->isVisible());
    const auto last = field->model()->index(field->count() - 1, 0);
    view->scrollTo(last);
    const auto lastRow = view->visualRect(last);
    QVERIFY(view->viewport()->rect().contains(lastRow.center()));
    QMouseEvent hover(QEvent::MouseMove, lastRow.center(), view->viewport()->mapToGlobal(lastRow.center()),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &hover);
    QCOMPARE(view->currentIndex(), last);
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    const auto layer = popup->grab();
    if (!preview.isEmpty()) {
        auto image = window.grab();
        QPainter painter(&image);
        painter.drawPixmap(window.mapFromGlobal(popup->mapToGlobal(QPoint{})), layer);
        painter.end();
        const auto filename = pairing ? "pairing-selector.png" : "invitation-selector.png";
        QVERIFY(image.save(QDir(preview).filePath(filename)));
        QVERIFY(layer.save(QDir(preview).filePath(pairing ? "pairing-layer.png" : "invitation-layer.png")));
    }
    const QRect panel(view->mapTo(popup, QPoint{}), view->size());
    verifyElevation(layer, panel);
    QVERIFY(view->viewport()->rect().contains(lastRow));
    QCOMPARE(panel.width(), field->width());
    QTest::qWait(QApplication::doubleClickInterval() + 20);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, lastRow.center());
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(field->currentData().toInt(), 720);
    QCOMPARE(changed.count(), 2);
    field->showPopup();
    QTRY_VERIFY(popup->isVisible());
    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, QPoint(2, panel.center().y()));
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(changed.count(), 2);
    field->showPopup();
    QTRY_VERIFY(popup->isVisible());
    const auto anchor = field->mapToGlobal(field->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, popup->mapFromGlobal(anchor), anchor,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(popup, &press);
    QTRY_VERIFY(!popup->isVisible());
    QVERIFY(popup->testAttribute(Qt::WA_NoMouseReplay));
    QCOMPARE(changed.count(), 2);
    if (pairing) {
        devices.setActivity(true, false);
        QVERIFY(!field->isEnabled());
        devices.setActivity(false, false);
        QVERIFY(field->isEnabled());
        devices.setRegistration(false, QStringLiteral("离线"));
        QVERIFY(!field->isEnabled());
        QCOMPARE(field->currentData().toInt(), 720);
    }
}

void DeviceSecurityProbe::floatingStopControl() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QVERIFY(window.setHostBusy(true, true));
    window.showMinimized();
    window.setHostActive(true);
    QWidget* panel = nullptr;
    QPushButton* stop = nullptr;
    for (auto* top : QApplication::topLevelWidgets()) {
        if (auto* button = top->findChild<QPushButton*>("trustedShareStop")) {
            panel = top;
            stop = button;
            break;
        }
    }
    QVERIFY(panel && stop);
    QTRY_VERIFY(panel->isVisible() && stop->isVisible());
    QVERIFY(panel->testAttribute(Qt::WA_DontShowOnScreen));
    QVERIFY(panel->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
    const auto image = panel->grab();
    verifyElevation(image, QRect(stop->mapTo(panel, QPoint{}), stop->size()));
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty())
        QVERIFY(image.save(QDir(preview).filePath("stop-sharing-layer.png")));
    QSignalSpy stopped(&window, &HomeWindow::hostEndRequested);
    QTest::mouseClick(stop, Qt::LeftButton);
    QCOMPARE(stopped.count(), 1);
    QVERIFY(window.setHostBusy(false));
    QVERIFY(!panel->isVisible());
}

void DeviceSecurityProbe::onscreenPopupOpening() {
    if (qEnvironmentVariableIsEmpty("EVP_PROBE_ONSCREEN_POPUP"))
        QSKIP("Set EVP_PROBE_ONSCREEN_POPUP=1 to exercise the Windows desktop compositor.");
    auto* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    QWidget fixture(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    fixture.setAttribute(Qt::WA_ShowWithoutActivating);
    fixture.setFixedSize(400, 380);
    fixture.setAutoFillBackground(true);
    const QColor background("#e3ecf7");
    auto colors = fixture.palette();
    colors.setColor(QPalette::Window, background);
    fixture.setPalette(colors);
    auto* layout = new QVBoxLayout(&fixture);
    layout->setContentsMargins(80, 24, 80, 24);
    auto* label = new QLabel(QStringLiteral("邀请码有效期"));
    layout->addWidget(label);
    auto* field = native_ui::codeLifetimeSelector(label, "probeLifetime");
    layout->addWidget(field);
    layout->addStretch();
    auto* popup = field->view()->window();
    const auto area = screen->availableGeometry();
    fixture.move(area.left() + (area.width() - fixture.width()) / 2,
                 area.top() + (area.height() - fixture.height()) / 2);
    fixture.show();
    QTest::qWait(150);
    const bool general = QApplication::isEffectEnabled(Qt::UI_General);
    const bool animated = QApplication::isEffectEnabled(Qt::UI_AnimateCombo);
    const auto restore = qScopeGuard([general, animated] {
        QApplication::setEffectEnabled(Qt::UI_AnimateCombo, animated);
        QApplication::setEffectEnabled(Qt::UI_General, general);
    });
    QApplication::setEffectEnabled(Qt::UI_General, true);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, true);
    const auto origin = fixture.mapToGlobal(QPoint{}) - screen->geometry().topLeft();
    const auto grab = [&] { return screen->grabWindow(0, origin.x(), origin.y(), fixture.width(), fixture.height()); };
    const auto baseline = grab();
    QVERIFY(!baseline.isNull());
    const auto ratio = baseline.devicePixelRatio();
    const auto sample = [&](const QImage& image, QPoint point) {
        return image.pixelColor(qRound(point.x() * ratio), qRound(point.y() * ratio));
    };
    QVERIFY(qAbs(sample(baseline.toImage(), QPoint(5, 5)).red() - background.red()) < 8);
    QTest::mouseClick(field, Qt::LeftButton);
    QElapsedTimer opening;
    opening.start();
    bool extraWindow = false, capturedDarkFrame = false;
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    do {
        QCoreApplication::processEvents();
        for (auto* top : QApplication::topLevelWidgets()) {
            if (top != &fixture && top != popup && top->isVisible())
                extraWindow = true;
        }
        const auto frame = grab();
        const auto image = frame.toImage();
        const QPoint surface = fixture.mapFromGlobal(popup->mapToGlobal(QPoint{}));
        const auto size = popup->size();
        const auto left = sample(image, surface + QPoint(3, size.height() / 2));
        const auto right = sample(image, surface + QPoint(size.width() - 4, size.height() / 2));
        const auto bottom = sample(image, surface + QPoint(size.width() / 2, size.height() - 4));
        const auto shadow = [](const QColor& color) { return qMax(color.red(), qMax(color.green(), color.blue())) < 135; };
        if (shadow(left) || shadow(right) || shadow(bottom)) {
            if (!capturedDarkFrame && !preview.isEmpty())
                frame.save(QDir(preview).filePath("opening-dark-frame.png"));
            capturedDarkFrame = true;
        }
        QTest::qWait(8);
    } while (opening.elapsed() < 230);
    const auto settled = grab();
    const auto bodyPoint = fixture.mapFromGlobal(popup->mapToGlobal(QPoint(popup->width() - 25, popup->height() / 2)));
    const auto body = sample(settled.toImage(), bodyPoint);
    if (!preview.isEmpty())
        QVERIFY(settled.save(QDir(preview).filePath("opening-settled.png")));
    QVERIFY(popup->isVisible());
    QVERIFY(QApplication::isEffectEnabled(Qt::UI_AnimateCombo));
    field->hidePopup();
    fixture.hide();
    QVERIFY(body.red() > 245 && body.green() > 245);
    QVERIFY2(!capturedDarkFrame, "The desktop compositor rendered a dark popup gutter.");
    QVERIFY2(!extraWindow, "An extra top-level animation window appeared while the dropdown opened.");
}

}

int runDeviceSecurityProbe(int argc, char** argv) {
    QApplication app(argc, argv);
    native_ui::applyTheme(native_ui::Theme::Light);
    native_ui::installFocusStyle(app);
    QTemporaryDir preferences;
    if (!preferences.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, preferences.filePath("system"));
    app.setOrganizationName("EVP-Smoke");
    app.setApplicationName("DeviceChecks");
    DeviceSecurityProbe tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "device_security_probe.moc"
