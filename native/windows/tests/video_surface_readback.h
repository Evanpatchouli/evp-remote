#pragma once

#include <QImage>

#include <cstdint>

namespace evp {

class VideoSurface;

struct VideoSurfaceReadback {
    static QImage render(VideoSurface& surface, QSize size, std::uint64_t* primitives = nullptr);
};

}  // namespace evp
