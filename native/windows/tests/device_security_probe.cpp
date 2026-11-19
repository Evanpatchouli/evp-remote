#include "controller.h"
#include "device_probe.h"
#include "devices/device_identity.h"
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <Windows.h>

namespace {

class DeviceSecurityProbe : public QObject {
    Q_OBJECT

private slots:
    void bindsProofToPeerAttemptNonceAndPayload();
    void pausesOnUnwritableSettingsAcrossRestarts();
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
