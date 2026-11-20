#include "update_release.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace evp {
namespace {

const auto downloadPath = QStringLiteral("/Evanpatchouli/evp-remote/releases/download/");
constexpr qint64 maximumAssetSize = 512 * 1024 * 1024;

bool secureUrl(const QUrl& url) {
    return url.isValid() && url.scheme() == "https" && url.port(443) == 443 &&
           url.userInfo().isEmpty() && !url.hasFragment();
}

}

QString updateAssetName(bool portable) {
    return portable ? QStringLiteral("evp-remote-windows-x64-portable.exe")
                    : QStringLiteral("evp-remote-windows-x64-setup.exe");
}

bool trustedUpdateRedirect(const QUrl& url) {
    const auto host = url.host();
    return secureUrl(url) && (host == "release-assets.githubusercontent.com" ||
                              host == "objects.githubusercontent.com");
}

std::optional<UpdateRelease> readUpdateRelease(const QByteArray& json, const QString& currentVersion, bool portable) {
    static const QRegularExpression stable(QStringLiteral(
        R"(\Av?((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:\+[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*)?)\z)"));
    static const QRegularExpression digestPattern(QStringLiteral(R"(\Asha256:([A-Fa-f0-9]{64})\z)"));
    const auto metadata = QJsonDocument::fromJson(json).object();
    const auto tag = metadata.value("tag_name").toString();
    const auto parsed = stable.match(tag);
    const auto version = parsed.captured(1);
    const auto releaseNumber = QVersionNumber::fromString(version);
    const auto currentNumber = QVersionNumber::fromString(currentVersion);
    if (!parsed.hasMatch() || releaseNumber.segmentCount() != 3 || currentNumber.segmentCount() != 3 ||
        metadata.value("draft") != false || metadata.value("prerelease") != false)
        return {};

    const bool newer = releaseNumber > currentNumber ||
                       (releaseNumber == currentNumber && currentVersion.section('+', 0, 0).contains('-'));
    UpdateRelease release{tag, version, metadata.value("body").toString().left(16000), newer};
    const auto name = updateAssetName(portable);
    for (const auto& entry : metadata.value("assets").toArray()) {
        const auto asset = entry.toObject();
        if (asset.value("name") != name || asset.value("state") != "uploaded")
            continue;
        const QUrl url(asset.value("browser_download_url").toString(), QUrl::StrictMode);
        const auto size = asset.value("size").toInteger(-1);
        const auto digest = digestPattern.match(asset.value("digest").toString());
        if (secureUrl(url) && url.host() == "github.com" && !url.hasQuery() &&
            url.path() == downloadPath + tag + '/' + name &&
            size >= 2 && size <= maximumAssetSize && digest.hasMatch()) {
            release.assetUrl = url;
            release.sha256 = QByteArray::fromHex(digest.captured(1).toLatin1());
            release.size = size;
            break;
        }
    }
    return release;
}

}
