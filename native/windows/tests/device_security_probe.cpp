#include "controller.h"
#include "device_probe.h"
#include "devices/device_identity.h"
#include "ui/focus_style.h"
#include "ui/select_field.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
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
    void lockedProfileRejectsFallbackJoin();
    void retriesFailedForgetWithoutEditing();
    void committedAddressSurvivesHeldMarker();
    void settingsEditorDiscardsDrafts();
    void guestJoinsWithAddressOnly();
    void storedKeyIsScopedToAddress();
    void readsLegacyConnectionSettings_data();
    void readsLegacyConnectionSettings();
    void assistanceTabsAndScrollableDeviceList();
    void cleanTabFocus();
    void cleanDeviceListFocus();
    void cleanCheckBoxFocus();
    void themedLifetimeSelectors_data();
    void themedLifetimeSelectors();
    void floatingStopControl();
    void onscreenPopupOpening();
};

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

void DeviceSecurityProbe::settingsEditorDiscardsDrafts() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    const device_probe::Ui ui(window);
    for (const auto* name : {"settingsNavigation", "settingsServerUrl", "settingsDeploymentKey",
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
    for (const auto* destination : {"navigation", "deviceNavigation"}) {
        ui.button("settingsNavigation")->click();
        QVERIFY(address->isVisible() && key->isVisible());
        QCOMPARE(QUrl(address->text()), committed);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        QVERIFY(key->text().isEmpty() && !forget->isChecked());
        key->setText("draft-for-previous-address");
        address->setText("ws://127.0.0.1:8882/");
        QVERIFY(key->text().isEmpty());
        key->setText("draft-only");
        ui.button(destination)->click();
        QVERIFY(!key->isVisible() && key->text().isEmpty());
    }
    QCOMPARE(saves.count(), 0);
    ui.button("settingsNavigation")->click();
    QCOMPARE(QUrl(address->text()), committed);
    QVERIFY(key->text().isEmpty());
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
    navigation->click();
    QCOMPARE(tabs->currentIndex(), 1);
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
    QVERIFY2(check->grab().toImage() != clicked, "Shortcut focus needs a visible cue on the checkbox.");
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
