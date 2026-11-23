#pragma once

#include "video_settings.h"

#include <QJsonValue>
#include <optional>

namespace evp {

inline std::optional<VideoAdaptation> parseVideoAdaptation(const QJsonValue& value) {
    const auto name = value.toString();
    if (name == "resolution") return VideoAdaptation::PreserveResolution;
    if (name == "balanced") return VideoAdaptation::Balanced;
    if (name == "framerate") return VideoAdaptation::PreserveFramerate;
    return std::nullopt;
}

inline QString videoAdaptationName(VideoAdaptation adaptation) {
    switch (adaptation) {
    case VideoAdaptation::PreserveResolution: return QStringLiteral("resolution");
    case VideoAdaptation::Balanced: return QStringLiteral("balanced");
    case VideoAdaptation::PreserveFramerate: return QStringLiteral("framerate");
    }
    return {};
}

} // namespace evp
