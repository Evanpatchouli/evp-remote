#include "device_identity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <cstring>
#include <utility>

namespace evp {
namespace {

constexpr auto base64Options = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
constexpr qsizetype privateSize = sizeof(BCRYPT_ECCKEY_BLOB) + 96;
constexpr qint64 maxStoreBytes = 128 * 1024;

QByteArray hash(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

PUCHAR data(const QByteArray& bytes) {
    return reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.constData()));
}

void erase(QByteArray& bytes) {
    if (!bytes.isEmpty())
        SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
}

QByteArray dpapi(const QByteArray& input, bool protect) {
    if (input.isEmpty())
        return {};
    QByteArray context("evp-device-store-v1");
    DATA_BLOB entropy{static_cast<DWORD>(context.size()), data(context)};
    DATA_BLOB source{static_cast<DWORD>(input.size()), data(input)}, output{};
    const bool ok = protect ? CryptProtectData(&source, L"EVP Remote devices", &entropy, nullptr, nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN, &output)
                            : CryptUnprotectData(&source, nullptr, &entropy, nullptr, nullptr,
                                                 CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
        return {};
    QByteArray result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    if (!protect)
        SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

}

QString encodeDeviceBytes(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toBase64(base64Options));
}

QByteArray decodeDeviceBytes(const QString& encoded, qsizetype size) {
    if (encoded.isEmpty() || encoded.size() > maxDevicePayload ||
        (size >= 0 && encoded.size() != (size * 8 + 5) / 6))
        return {};
    const auto result = QByteArray::fromBase64Encoding(encoded.toLatin1(), base64Options |
                                                       QByteArray::AbortOnBase64DecodingErrors);
    if (!result || (size >= 0 && result.decoded.size() != size) || encodeDeviceBytes(result.decoded) != encoded)
        return {};
    return result.decoded;
}

QString deviceId(const QByteArray& publicKey) {
    return publicKey.size() == 64 ? encodeDeviceBytes(hash(publicKey)) : QString{};
}

QByteArray deviceRandom() {
    QByteArray bytes(32, Qt::Uninitialized);
    if (BCryptGenRandom(nullptr, data(bytes), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {};
    return bytes;
}

QByteArray deviceProof(std::initializer_list<QString> fields) {
    QByteArray result("evp-device-v1");
    for (const auto& field : fields) {
        result.append('\n');
        result.append(field.toUtf8());
    }
    return result;
}

QString deviceName(QString name) {
    name.truncate(80);
    for (auto& character : name) {
        if (character.category() == QChar::Other_Control || character.category() == QChar::Other_Format ||
            character.category() == QChar::Separator_Line || character.category() == QChar::Separator_Paragraph)
            character = QChar(' ');
    }
    name = name.simplified();
    return name.isEmpty() ? QStringLiteral("Windows 设备") : name;
}

bool equalDeviceSecrets(const QByteArray& left, const QByteArray& right) {
    if (left.size() != 32 || right.size() != 32)
        return false;
    unsigned difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index)
        difference |= static_cast<unsigned char>(left.at(index)) ^ static_cast<unsigned char>(right.at(index));
    return difference == 0;
}

QUrl deviceServerUrl(QUrl url) {
    const auto host = url.host();
    const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "::1";
    if (!url.isValid() || host.isEmpty() || (url.scheme() != "wss" && !(url.scheme() == "ws" && loopback)) ||
        url.authority().contains('@'))
        return {};
    if (url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/"))
        return {};
    if (url.port() == (url.scheme() == "wss" ? 443 : 80))
        url.setPort(-1);
    url.setPath("/");
    return url;
}

struct DeviceIdentity::Key {
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_KEY_HANDLE value = nullptr;

    Key() {
        BCryptOpenAlgorithmProvider(&provider, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
    }

    ~Key() {
        if (value)
            BCryptDestroyKey(value);
        if (provider)
            BCryptCloseAlgorithmProvider(provider, 0);
    }

    QByteArray exportBlob(LPCWSTR kind, ULONG size) const {
        if (!value)
            return {};
        QByteArray bytes(size, Qt::Uninitialized);
        ULONG written = 0;
        if (BCryptExportKey(value, nullptr, kind, data(bytes), size, &written, 0) < 0 || written != size) {
            erase(bytes);
            return {};
        }
        return bytes;
    }
};

DeviceIdentity::DeviceIdentity() = default;
DeviceIdentity::~DeviceIdentity() = default;

bool DeviceIdentity::load(const QByteArray& privateKey) {
    auto candidate = std::make_unique<Key>();
    if (!candidate->provider)
        return false;
    if (privateKey.isEmpty()) {
        if (BCryptGenerateKeyPair(candidate->provider, &candidate->value, 256, 0) < 0 ||
            BCryptFinalizeKeyPair(candidate->value, 0) < 0)
            return false;
    } else if (privateKey.size() != privateSize ||
               BCryptImportKeyPair(candidate->provider, nullptr, BCRYPT_ECCPRIVATE_BLOB, &candidate->value,
                                   data(privateKey), static_cast<ULONG>(privateKey.size()), 0) < 0) {
        return false;
    }
    key_ = std::move(candidate);
    return true;
}

QByteArray DeviceIdentity::privateKey() const {
    return key_ ? key_->exportBlob(BCRYPT_ECCPRIVATE_BLOB, privateSize) : QByteArray{};
}

QByteArray DeviceIdentity::publicKey() const {
    return key_ ? key_->exportBlob(BCRYPT_ECCPUBLIC_BLOB, sizeof(BCRYPT_ECCKEY_BLOB) + 64)
                      .mid(sizeof(BCRYPT_ECCKEY_BLOB))
                : QByteArray{};
}

QString DeviceIdentity::sign(const QByteArray& proof) const {
    if (!key_)
        return {};
    const auto digest = hash(proof);
    QByteArray signature(64, Qt::Uninitialized);
    ULONG written = 0;
    if (BCryptSignHash(key_->value, nullptr, data(digest), 32, data(signature), 64, &written, 0) < 0 || written != 64)
        return {};
    return encodeDeviceBytes(signature);
}

bool DeviceIdentity::verify(const QByteArray& publicKey, const QByteArray& proof, const QString& signature) {
    const auto raw = decodeDeviceBytes(signature, 64);
    if (publicKey.size() != 64 || raw.isEmpty())
        return false;
    Key key;
    if (!key.provider)
        return false;
    const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
    QByteArray blob(reinterpret_cast<const char*>(&header), sizeof(header));
    blob.append(publicKey);
    if (BCryptImportKeyPair(key.provider, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.value,
                            data(blob), static_cast<ULONG>(blob.size()), 0) < 0)
        return false;
    const auto digest = hash(proof);
    return BCryptVerifySignature(key.value, nullptr, data(digest), 32, data(raw), 64, 0) == 0;
}

DeviceStore::DeviceStore(QString path) : path_(std::move(path)) {
    if (path_.isEmpty()) {
        const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!base.isEmpty())
            path_ = QDir(base).filePath("devices.dat");
    }
    const auto location = QFileInfo(path_).absoluteFilePath().toCaseFolded().toUtf8();
    holdSetting_ = "device-storage-hold/" + QString::fromLatin1(hash(location).toHex());
}

DeviceStore::~DeviceStore() = default;

bool DeviceStore::hasStorageHold() const {
    return QFileInfo::exists(path_ + ".hold") || QSettings().value(holdSetting_).toBool();
}

bool DeviceStore::markStorageHold() const {
    if (!lock_ || !lock_->isLocked())
        return false;
    QFile marker(path_ + ".hold");
    if (marker.exists() || marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return true;
    QSettings settings;
    settings.setValue(holdSetting_, true);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool DeviceStore::open() {
    if (path_.isEmpty() || !QDir().mkpath(QFileInfo(path_).absolutePath()))
        return false;
    lock_ = std::make_unique<QLockFile>(path_ + ".lock");
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock())
        return false;
    QFile file(path_);
    if (!file.exists())
        return identity.load({}) && save();
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxStoreBytes)
        return false;
    auto plain = dpapi(file.readAll(), false);
    if (plain.size() <= privateSize) {
        erase(plain);
        return false;
    }
    auto privateKey = plain.left(privateSize);
    const auto document = QJsonDocument::fromJson(plain.mid(privateSize));
    erase(plain);
    const auto state = document.object();
    const bool keyLoaded = state.value("version") == 1 && identity.load(privateKey);
    erase(privateKey);
    if (!keyLoaded || !state.value("peers").isArray())
        return false;
    const auto entries = state.value("peers").toArray();
    if (entries.size() > maxDevicePeers)
        return false;
    for (const auto& entry : entries) {
        const auto item = entry.toObject();
        const auto key = decodeDeviceBytes(item.value("publicKey").toString(), 64);
        if (key.isEmpty())
            return false;
        peers.insert(deviceId(key), {key, deviceName(item.value("name").toString())});
    }
    const auto savedUrl = state.value("url").toString();
    url = deviceServerUrl(QUrl(savedUrl, QUrl::StrictMode));
    deploymentKey = state.value("key").toString();
    if ((!savedUrl.isEmpty() && url.isEmpty()) || url.toEncoded().size() > 2048 ||
        deploymentKey.toUtf8().size() > 4096 || (url.isEmpty() && !deploymentKey.isEmpty()))
        return false;
    preferences = {state.value("allowIncoming").toBool(), state.value("audio").toBool(true),
                   state.value("relayOnly").toBool()};
    return true;
}

DeviceStore::WriteResult DeviceStore::write() const {
    if (!lock_ || !lock_->isLocked())
        return WriteResult::Failed;
    auto plain = identity.privateKey();
    if (plain.size() != privateSize)
        return WriteResult::Failed;
    QJsonArray entries;
    for (const auto& peer : peers)
        entries.append(QJsonObject{{"publicKey", encodeDeviceBytes(peer.publicKey)}, {"name", peer.name}});
    const QJsonObject state{{"version", 1}, {"peers", entries}, {"url", url.toString(QUrl::FullyEncoded)},
                            {"key", deploymentKey}, {"allowIncoming", preferences.allowIncoming},
                            {"audio", preferences.audio}, {"relayOnly", preferences.relayOnly}};
    plain.append(QJsonDocument(state).toJson(QJsonDocument::Compact));
    const auto encrypted = dpapi(plain, true);
    erase(plain);
    if (encrypted.isEmpty() || encrypted.size() > maxStoreBytes)
        return WriteResult::Failed;
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size() || !file.commit())
        return WriteResult::Failed;
    const auto marker = path_ + ".hold";
    if (QFileInfo::exists(marker) && !QFile::remove(marker))
        return WriteResult::Held;
    QSettings settings;
    if (!settings.contains(holdSetting_))
        return WriteResult::Saved;
    settings.remove(holdSetting_);
    settings.sync();
    return settings.status() == QSettings::NoError ? WriteResult::Saved : WriteResult::Held;
}

}
