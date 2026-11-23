#include "video_surface_readback.h"
#include "../media/video_surface.h"

#include <QApplication>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

using evp::VideoSurface;
using evp::VideoSurfaceReadback;
using Microsoft::WRL::ComPtr;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool colorWithinTolerance(int actual, int expected, int tolerance = 2) {
    return std::abs(actual - expected) <= tolerance;
}

void checkDownsampling(VideoSurface& surface) {
    QImage source(90, 60, QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            // Red is a one-pixel T. Green and blue carry horizontal and
            // vertical stripes at the source Nyquist limit.
            const bool stroke = (y == 20 && x >= 40 && x <= 48)
                || (x == 44 && y > 20 && y <= 29);
            source.setPixel(x, y, qRgb(stroke ? 255 : 0, y % 2 * 255, x % 2 * 255));
        }
    }
    const auto render = [&](QSize size, std::uint64_t* primitives = nullptr) {
        require(surface.submitBgra(source.width(), source.height(), source.constBits(),
                                   static_cast<int>(source.bytesPerLine())), "BGRA submit failed");
        const auto result = VideoSurfaceReadback::render(surface, size, primitives);
        require(result.size() == size, "BGRA readback failed");
        return result;
    };

    const auto exact = render(source.size());
    require(exact == source, "native-size pixels changed");

    const auto reduced = render({54, 36});
    for (int y = 3; y < reduced.height() - 3; ++y) {
        for (int x = 3; x < reduced.width() - 3; ++x) {
            const auto pixel = reduced.pixel(x, y);
            require(colorWithinTolerance(qGreen(pixel), 128) && colorWithinTolerance(qBlue(pixel), 128),
                    "downsample aliased stripe pattern");
        }
    }
    require(qRed(reduced.pixel(26, 12)) > 110 && qRed(reduced.pixel(26, 15)) > 110
            && qRed(reduced.pixel(23, 15)) < 8, "one-pixel text strokes lost contrast");

    std::uint64_t downsamplePrimitives = 0;
    const auto severe = render({9, 6}, &downsamplePrimitives);
    require(downsamplePrimitives > 2, "severe downsample did not use the prefilter");
    for (int y = 0; y < severe.height(); ++y) {
        for (int x = 0; x < severe.width(); ++x) {
            const auto pixel = severe.pixel(x, y);
            require(colorWithinTolerance(qGreen(pixel), 128) && colorWithinTolerance(qBlue(pixel), 128),
                    "severe downsample aliased stripe pattern");
        }
    }
    std::uint64_t redrawPrimitives = 0;
    const auto redraw = VideoSurfaceReadback::render(surface, severe.size(), &redrawPrimitives);
    require(redraw == severe && redrawPrimitives == 1,
            "unchanged-frame redraw did not reuse the filtered first axis");
}

void checkColor(const QImage& image, QSize size, QRgb expected) {
    require(image.size() == size, "YUV readback failed");
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixel(x, y);
            require(colorWithinTolerance(qRed(pixel), qRed(expected)) && colorWithinTolerance(qGreen(pixel), qGreen(expected))
                    && colorWithinTolerance(qBlue(pixel), qBlue(expected)) && qAlpha(pixel) == 255,
                    "YUV matrix, range or visible edge changed");
        }
    }
}

void checkYuv(VideoSurface& surface) {
    constexpr int width = 51, height = 27;
    constexpr int chromaWidth = (width + 1) / 2, chromaHeight = (height + 1) / 2;
    constexpr unsigned char luma = 92, u = 101, v = 177;
    const std::vector<unsigned char> yPlane(width * height, luma);
    const std::vector<unsigned char> uPlane(chromaWidth * chromaHeight, u);
    const std::vector<unsigned char> vPlane(chromaWidth * chromaHeight, v);
    require(surface.submitI420(width, height, yPlane.data(), width, uPlane.data(), chromaWidth,
                               vPlane.data(), chromaWidth, 180, {}, {VideoSurface::Matrix::Bt2020, false}),
            "I420 submit failed");
    checkColor(VideoSurfaceReadback::render(surface, {17, 9}), {17, 9}, qRgb(171, 62, 31));

    ComPtr<ID3D11Device> device;
    device.Attach(surface.acquireD3D11Device());
    require(device != nullptr, "D3D11 device unavailable");
    UINT flags = 0;
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &flags))
        || !(flags & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
        std::puts("Padded NV12 cases skipped because the adapter lacks NV12 textures.");
        return;
    }

    constexpr int paddedWidth = 64, paddedHeight = 40;
    std::vector<unsigned char> nv12(paddedWidth * paddedHeight * 3 / 2, 3);
    for (int y = 0; y < height; ++y) std::fill_n(nv12.data() + y * paddedWidth, width, luma);
    auto* chroma = nv12.data() + paddedWidth * paddedHeight;
    for (int y = 0; y < chromaHeight; ++y) {
        for (int x = 0; x < chromaWidth; ++x) {
            chroma[y * paddedWidth + x * 2] = u;
            chroma[y * paddedWidth + x * 2 + 1] = v;
        }
    }
    const std::vector<unsigned char> unusedSlice(nv12.size(), 85);
    const D3D11_SUBRESOURCE_DATA data[]{{unusedSlice.data(), paddedWidth, 0},
                                        {nv12.data(), paddedWidth, 0}};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = paddedWidth;
    desc.Height = paddedHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 2;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(device->CreateTexture2D(&desc, data, &texture)), "NV12 texture creation failed");
    for (const QSize size : {QSize(18, 34), QSize(9, 17), QSize(54, 102)}) {
        require(surface.submitNv12(width, height, texture.Get(), 1, std::make_shared<int>(0),
                                   {VideoSurface::Matrix::Bt709, true}, 90), "NV12 submit failed");
        checkColor(VideoSurfaceReadback::render(surface, size), size, qRgb(169, 74, 42));
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        VideoSurface surface;
        checkDownsampling(surface);
        checkYuv(surface);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("Video surface readback passed.");
    return 0;
}
