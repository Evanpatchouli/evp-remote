#include "controller.h"
#include "device_probe.h"
#include "devices/device_identity.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <Windows.h>

namespace {

class DeviceSecurityProbe : public QObject {
    Q_OBJECT

private slots:
    void bindsProofToPeerAttemptNonceAndPayload();
    void pausesOnUnwritableSettingsAcrossRestarts();
    void assistanceTabsAndScrollableDeviceList();
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
    const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
    const auto open = CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(open != INVALID_HANDLE_VALUE);
    const auto close = [](void* handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close)> heldFile(open, close);
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        const device_probe::Ui ui(window);
        QCOMPARE(ui.id(), id);
        QVERIFY(!ui.online());
        QVERIFY(!window.findChild<QCheckBox*>("allowTrustedControl")->isChecked());
        QVERIFY(ui.message().contains(QStringLiteral("尚未保存")));
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
    QCOMPARE(tabs->currentIndex(), 0);
    window.setSignalingUrl(QUrl("wss://first.example.invalid/"));
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
    auto* key = window.findChild<QLineEdit*>("hostKey");
    key->setText("ui-test-credential");
    tabs->setCurrentIndex(0);
    window.findChild<QLineEdit*>("signalingUrl")->setText("wss://second.example.invalid/");
    tabs->setCurrentIndex(1);
    QVERIFY(key->text().isEmpty());
    window.setSignalingUrl({});
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

}

int runDeviceSecurityProbe(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir preferences;
    if (!preferences.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    app.setOrganizationName("EVP-Smoke");
    app.setApplicationName("DeviceChecks");
    DeviceSecurityProbe tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "device_security_probe.moc"
