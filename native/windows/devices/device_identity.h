#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <initializer_list>
#include <memory>

class QLockFile;

namespace evp {

inline constexpr int maxDevicePeers = 64;
inline constexpr int maxDevicePayload = 56 * 1024;
inline constexpr int maxDeviceNameLength = 80;

QByteArray decodeDeviceBytes(const QString& encoded, qsizetype size = -1);
QString encodeDeviceBytes(const QByteArray& bytes);
QString deviceId(const QByteArray& publicKey);
QByteArray deviceRandom();
QByteArray deviceProof(std::initializer_list<QString> fields);
QString normalizedDeviceName(QString name);
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
    QString alias;
    qsizetype order = 0;
    bool pinned = false;

    QString displayName() const { return alias.isEmpty() ? name : alias; }
};

enum class DeviceMove { Up, Down };

QStringList orderedDeviceIds(const QMap<QString, DevicePeer>& peers);

struct DevicePreferences {
    bool allowIncoming = false;
    bool audio = true;
    bool relayOnly = false;
};

class DeviceStore {
public:
    enum class WriteResult { Failed, Held, Saved };
    explicit DeviceStore(QString path);
    ~DeviceStore();
    bool open();
    WriteResult write() const;
    bool save() const { return write() == WriteResult::Saved; }
    bool savePeerPresentation(QMap<QString, DevicePeer> updatedPeers);
    bool hasStorageHold() const;
    bool markStorageHold() const;

    DeviceIdentity identity;
    QMap<QString, DevicePeer> peers;
    DevicePreferences preferences;
    QUrl url;
    QString deploymentKey;

private:
    bool writeProfile(const QMap<QString, DevicePeer>& savedPeers) const;

    QString path_;
    QString holdSetting_;
    std::unique_ptr<QLockFile> lock_;
};

}
