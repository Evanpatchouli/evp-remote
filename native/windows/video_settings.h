#pragma once

namespace evp {

struct VideoSettings {
    int maxBitrateMbps = 8;
    int maxFps = 60;

    bool valid() const {
        return maxBitrateMbps >= 0 && maxBitrateMbps <= 50 &&
               (maxFps == 30 || maxFps == 60 || maxFps == 90 || maxFps == 144);
    }

    bool operator==(const VideoSettings&) const = default;
};

}
