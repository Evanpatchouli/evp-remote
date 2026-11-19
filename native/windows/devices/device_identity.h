#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>
#include <initializer_list>
#include <memory>

class QLockFile;

namespace evp {

inline constexpr int maxDevicePeers = 64;
inline constexpr int maxDevicePayload = 56 * 1024;

QByteArray decodeDeviceBytes(const QString& encoded, qsizetype size = -1);
QString encodeDeviceBytes(const QByteArray& bytes);
QString deviceId(const QByteArray& publicKey);
QByteArray deviceRandom();
QByteArray deviceProof(std::initializer_list<QString> fields);
QString deviceName(QString name);
bool equalDeviceSecrets(const QByteArray& left, const QByteArray& right);
QUrl deviceServerUrl(QUrl url);

class DeviceIdentity {
public:
    DeviceIdentity();
    ~DeviceIdentity();
    bool load(const QByteArray& privateKey);
    QByteArray privateKey() const;
    QByteArray publicKey() const;
    QString sign(const QByteArray& proof) const;
    static bool verify(const QByteArray& publicKey, const QByteArray& proof, const QString& signature);

private:
    struct Key;
    std::unique_ptr<Key> key_;
};

struct DevicePeer {
    QByteArray publicKey;
    QString name;
};

struct DevicePreferences {
    bool allowIncoming = false;
    bool audio = true;
    bool highRefresh = false;
    bool relayOnly = false;
};

class DeviceStore {
public:
    explicit DeviceStore(QString path);
    ~DeviceStore();
    bool open();
    bool save() const;
    bool hasStorageHold() const;
    bool markStorageHold() const;

    DeviceIdentity identity;
    QMap<QString, DevicePeer> peers;
    DevicePreferences preferences;
    QUrl url;
    QString deploymentKey;

private:
    QString path_;
    QString holdSetting_;
    std::unique_ptr<QLockFile> lock_;
};

}
