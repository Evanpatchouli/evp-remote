#pragma once

namespace evp {

enum class VideoAdaptation {
    PreserveResolution,
    Balanced,
    PreserveFramerate,
};

struct VideoSettings {
    int maxBitrateMbps = 8;
    int maxFps = 60;
    VideoAdaptation adaptation = VideoAdaptation::PreserveResolution;

    bool valid() const {
        return maxBitrateMbps >= 0 && maxBitrateMbps <= 50 &&
               (maxFps == 30 || maxFps == 60 || maxFps == 90 || maxFps == 144) &&
               (adaptation == VideoAdaptation::PreserveResolution || adaptation == VideoAdaptation::Balanced ||
                adaptation == VideoAdaptation::PreserveFramerate);
    }

    bool operator==(const VideoSettings&) const = default;
};

}
