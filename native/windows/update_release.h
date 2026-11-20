#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

namespace evp {

struct UpdateRelease {
    QString tag;
    QString version;
    QString notes;
    bool newer = false;
    QUrl assetUrl;
    QByteArray sha256;
    qint64 size = 0;

    bool matchesAsset(const UpdateRelease& other) const {
        return tag == other.tag && assetUrl == other.assetUrl && sha256 == other.sha256 && size == other.size;
    }
};

QString updateAssetName(bool portable);
bool trustedUpdateRedirect(const QUrl& url);
std::optional<UpdateRelease> readUpdateRelease(const QByteArray& json, const QString& currentVersion, bool portable);

}
