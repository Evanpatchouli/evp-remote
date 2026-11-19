#pragma once

#include <QString>
#include <algorithm>
#include <array>

namespace evp {

inline constexpr std::array<int, 7> codeLifetimeMinutes{5, 15, 30, 60, 120, 300, 720};

inline constexpr bool isCodeLifetime(int minutes) {
    return std::find(codeLifetimeMinutes.begin(), codeLifetimeMinutes.end(), minutes) != codeLifetimeMinutes.end();
}

inline QString codeLifetimeLabel(int minutes) {
    return minutes < 60 ? QStringLiteral("%1 分钟").arg(minutes) : QStringLiteral("%1 小时").arg(minutes / 60);
}

struct CodeOptions {
    int minutes = codeLifetimeMinutes.front();
    bool autoApprove = false;
};

}
