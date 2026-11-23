#include "desktop_capture.h"
#include "../media/video_timing.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <webrtc_c/api/video/i420_buffer.h>
#include <webrtc_c/api/video/adapted_video_track_source.h>
#include <webrtc_c/rtc_base/time_utils.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace evp {
namespace {

using Microsoft::WRL::ComPtr;
struct ReleaseFrameBuffer {
    void operator()(webrtc_VideoFrameBuffer_refcounted* retained) const {
        webrtc_VideoFrameBuffer_Release(webrtc_VideoFrameBuffer_refcounted_get(retained));
    }
};
using FrameBuffer = std::unique_ptr<webrtc_VideoFrameBuffer_refcounted, ReleaseFrameBuffer>;

void require(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(QStringLiteral("%1 (0x%2)")
            .arg(QString::fromLatin1(operation)).arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'))
            .toStdString());
    }
}

struct AcquiredFrame {
    IDXGIOutputDuplication* output;
    ~AcquiredFrame() { output->ReleaseFrame(); }
};

struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ~MappedTexture() { context->Unmap(texture, 0); }
};

struct Pointer {
    void update(IDXGIOutputDuplication* output, const DXGI_OUTDUPL_FRAME_INFO& frame) {
        if (frame.LastMouseUpdateTime.QuadPart) position = frame.PointerPosition;
        if (!frame.PointerShapeBufferSize) return;
        if (frame.PointerShapeBufferSize > 1024 * 1024) throw std::runtime_error("DXGI pointer shape is too large");
        pixels.resize(frame.PointerShapeBufferSize);
        UINT required = 0;
        require(output->GetFramePointerShape(static_cast<UINT>(pixels.size()), pixels.data(), &required, &shape),
                "DXGI GetFramePointerShape");
    }

    bool visible() const { return position.Visible && !pixels.empty(); }

    void draw(std::uint8_t* output, int width, int height, int pitch) const {
        const bool monochrome = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const int shapeWidth = static_cast<int>(shape.Width);
        const int shapeHeight = static_cast<int>(monochrome ? shape.Height / 2 : shape.Height);
        if (shapeWidth <= 0 || shapeHeight <= 0 || !shape.Pitch ||
            shape.Height > pixels.size() / shape.Pitch ||
            (monochrome ? (shape.Width + 7u) / 8u > shape.Pitch : shape.Width > shape.Pitch / 4u)) return;

        const int left = position.Position.x;
        const int top = position.Position.y;
        for (int row = std::max(0, -top); row < std::min(shapeHeight, height - top); ++row) {
            for (int column = std::max(0, -left); column < std::min(shapeWidth, width - left); ++column) {
                auto* dst = output + static_cast<size_t>(top + row) * pitch + (left + column) * 4;
                const auto* src = pixels.data() + static_cast<size_t>(row) * shape.Pitch;
                if (monochrome) {
                    const auto bit = 0x80 >> (column & 7);
                    const auto andMask = src[column / 8] & bit ? 255 : 0;
                    const auto xorMask = src[static_cast<size_t>(shapeHeight) * shape.Pitch + column / 8] & bit ? 255 : 0;
                    for (int c = 0; c < 3; ++c) dst[c] = static_cast<std::uint8_t>((dst[c] & andMask) ^ xorMask);
                } else {
                    src += column * 4;
                    if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                        for (int c = 0; c < 3; ++c) dst[c] = src[3] ? dst[c] ^ src[c] : src[c];
                    } else {
                        for (int c = 0; c < 3; ++c) {
                            dst[c] = static_cast<std::uint8_t>((src[c] * src[3] + dst[c] * (255 - src[3]) + 127) / 255);
                        }
                    }
                }
            }
        }
    }

    DXGI_OUTDUPL_POINTER_POSITION position{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    std::vector<std::uint8_t> pixels;
};

struct Duplication {
    Duplication() {
        const auto monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
        ComPtr<IDXGIFactory1> factory;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
            require(adapterResult, "DXGI EnumAdapters1");
            for (UINT outputIndex = 0;; ++outputIndex) {
                ComPtr<IDXGIOutput> output;
                const auto outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND) break;
                require(outputResult, "DXGI EnumOutputs");
                DXGI_OUTPUT_DESC description{};
                require(output->GetDesc(&description), "DXGI GetDesc");
                if (!description.AttachedToDesktop || description.Monitor != monitor) continue;
                require(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                          nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
                        "D3D11CreateDevice");
                ComPtr<IDXGIOutput1> selected;
                require(output.As(&selected), "DXGI IDXGIOutput1");
                require(selected->DuplicateOutput(device.Get(), &output_), "DXGI DuplicateOutput");
                output_->GetDesc(&mode);
                return;
            }
        }
        throw std::runtime_error("No DXGI primary output");
    }

    bool rotated() const {
        return mode.Rotation == DXGI_MODE_ROTATION_ROTATE90 || mode.Rotation == DXGI_MODE_ROTATION_ROTATE270;
    }

    FrameBuffer read(ID3D11Texture2D* texture, const std::function<void(int, int)>& onSize) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (!staging) {
            sourceWidth = static_cast<int>(description.Width);
            sourceHeight = static_cast<int>(description.Height);
            width = rotated() ? sourceHeight : sourceWidth;
            height = rotated() ? sourceWidth : sourceHeight;
            if (width < 2 || height < 2 || description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                throw std::runtime_error("Unsupported DXGI frame format");
            }

            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.MiscFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            require(device->CreateTexture2D(&description, nullptr, &staging), "D3D11 CreateTexture2D");
            conversion.reset(sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                            width & ~1, height & ~1, AV_PIX_FMT_YUV420P,
                                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
            if (!conversion) throw std::runtime_error("FFmpeg BGRA conversion unavailable");
            const auto* matrix = sws_getCoefficients(SWS_CS_ITU709);
            if (sws_setColorspaceDetails(conversion.get(), matrix, 1, matrix, 0, 0, 1 << 16, 1 << 16) < 0) {
                throw std::runtime_error("FFmpeg BT.709 conversion unavailable");
            }
            if (onSize) onSize(width, height);
        } else if (static_cast<int>(description.Width) != sourceWidth ||
                   static_cast<int>(description.Height) != sourceHeight ||
                   description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            throw std::runtime_error("DXGI source mode changed");
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(readbackTime.measure([&] {
            context->CopyResource(staging.Get(), texture);
            return context->Map(staging.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
        }), "D3D11 Map");
        const MappedTexture unmap{context.Get(), staging.Get()};
        auto* pixels = static_cast<std::uint8_t*>(mapped.pData);
        int pitch = static_cast<int>(mapped.RowPitch);
        if (mode.Rotation > DXGI_MODE_ROTATION_IDENTITY) {
            orient(pixels, pitch);
            pixels = oriented.data();
            pitch = width * 4;
        }
        if (pointer.visible()) pointer.draw(pixels, width, height, pitch);

        auto* retained = webrtc_I420Buffer_CreateWithStrides(width & ~1, height & ~1,
            (width + 31) & ~31, (width / 2 + 31) & ~31, (width / 2 + 31) & ~31);
        if (!retained) throw std::runtime_error("WebRTC frame allocation failed");
        auto* i420 = webrtc_I420Buffer_refcounted_get(retained);
        FrameBuffer buffer(webrtc_I420Buffer_refcounted_cast_to_webrtc_VideoFrameBuffer(retained));
        webrtc_I420Buffer_Release(i420);
        std::uint8_t* dst[] = {webrtc_I420Buffer_MutableDataY(i420), webrtc_I420Buffer_MutableDataU(i420),
                               webrtc_I420Buffer_MutableDataV(i420), nullptr};
        const int dstPitch[] = {webrtc_I420Buffer_StrideY(i420), webrtc_I420Buffer_StrideU(i420),
                                webrtc_I420Buffer_StrideV(i420), 0};
        const std::uint8_t* src[] = {pixels, nullptr, nullptr, nullptr};
        const int srcPitch[] = {pitch, 0, 0, 0};
        if (conversionTime.measure([&] {
            return sws_scale(conversion.get(), src, srcPitch, 0, height, dst, dstPitch);
        }) != (height & ~1)) {
            throw std::runtime_error("FFmpeg BGRA conversion failed");
        }
        return buffer;
    }

    void orient(const std::uint8_t* pixels, int pitch) {
        oriented.resize(static_cast<size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y) {
            auto* dst = oriented.data() + static_cast<size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                int srcX = width - 1 - x;
                int srcY = height - 1 - y;
                if (mode.Rotation == DXGI_MODE_ROTATION_ROTATE90) {
                    srcX = y;
                    srcY = width - 1 - x;
                } else if (mode.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
                    srcX = height - 1 - y;
                    srcY = x;
                }
                std::memcpy(dst + x * 4, pixels + static_cast<size_t>(srcY) * pitch + srcX * 4, 4);
            }
        }
    }

    ComPtr<IDXGIOutputDuplication> output_;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    VideoStageTiming readbackTime{"desktop-readback"};
    VideoStageTiming conversionTime{"bgra-to-i420"};
    DXGI_OUTDUPL_DESC mode{};
    Pointer pointer;
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> conversion{nullptr, sws_freeContext};
    std::vector<std::uint8_t> oriented;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int width = 0;
    int height = 0;
};

void submit(webrtc_AdaptedVideoTrackSource* source, webrtc_VideoFrameBuffer_refcounted* retained,
            std::int64_t timestamp, bool repeated) {
    auto* buffer = webrtc_VideoFrameBuffer_refcounted_get(retained);
    const auto width = webrtc_VideoFrameBuffer_width(buffer);
    const auto height = webrtc_VideoFrameBuffer_height(buffer);
    int adaptedWidth = 0;
    int adaptedHeight = 0;
    int cropWidth = 0;
    int cropHeight = 0;
    int cropX = 0;
    int cropY = 0;
    if (!webrtc_AdaptedVideoTrackSource_AdaptFrame(source, width, height, timestamp,
                                                 &adaptedWidth, &adaptedHeight,
                                                 &cropWidth, &cropHeight, &cropX, &cropY)) return;
    adaptedWidth &= ~1;
    adaptedHeight &= ~1;
    if (adaptedWidth < 2 || adaptedHeight < 2) return;
    // Keep the entire desktop visible so normalized input still maps to the physical output.
    FrameBuffer scaled;
    if (adaptedWidth != width || adaptedHeight != height) {
        scaled.reset(webrtc_VideoFrameBuffer_Scale(buffer, adaptedWidth, adaptedHeight));
        if (!scaled) throw std::runtime_error("WebRTC frame scaling failed");
        retained = scaled.get();
    }
    const std::unique_ptr<webrtc_VideoFrameBuilder_unique, decltype(&webrtc_VideoFrameBuilder_unique_delete)> builder(
        webrtc_VideoFrameBuilder_new(retained), webrtc_VideoFrameBuilder_unique_delete);
    if (!builder) throw std::runtime_error("WebRTC frame builder allocation failed");
    auto* spec = webrtc_VideoFrameBuilder_unique_get(builder.get());
    webrtc_VideoFrameBuilder_set_timestamp_us(spec, timestamp);
    webrtc_VideoFrameBuilder_set_is_repeat_frame(spec, repeated);
    const std::unique_ptr<webrtc_VideoFrame_unique, decltype(&webrtc_VideoFrame_unique_delete)> frame(
        webrtc_VideoFrameBuilder_build(spec), webrtc_VideoFrame_unique_delete);
    webrtc_AdaptedVideoTrackSource_OnFrame(source, webrtc_VideoFrame_unique_get(frame.get()));
}

}  // namespace

void DesktopCapture::run(std::stop_token stop, webrtc_AdaptedVideoTrackSource* source,
                         const std::function<void(int, int)>& onSize) {
    Duplication duplication;
    FrameBuffer last;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto startMicros = webrtc_TimeMillis() * 1'000;
    const auto nowMicros = [&] {
        return startMicros + std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    };
    int fps = 0;
    int interval = 0;
    std::int64_t nextCapture = 0;
    std::int64_t lastSubmit = 0;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(cadenceMutex_);
            if (fps != maxFps_) {
                fps = maxFps_;
                interval = 1'000'000 / fps;
                if (lastSubmit) nextCapture = lastSubmit + interval;
            }
            const auto untilDue = nextCapture - nowMicros() - 750;
            if (untilDue > 0 && cadenceChanged_.wait_for(lock, stop, std::chrono::microseconds(untilDue),
                                                       [this, fps] { return maxFps_ != fps; })) {
                continue;
            }
        }
        if (stop.stop_requested()) break;
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const auto result = duplication.output_->AcquireNextFrame(12, &info, &resource);
        const auto timestamp = nowMicros();
        if (result == DXGI_ERROR_WAIT_TIMEOUT) {
            if (last && timestamp - lastSubmit >= 250'000 && !stop.stop_requested()) {
                submit(source, last.get(), timestamp, true);
                lastSubmit = timestamp;
            }
            continue;
        }
        require(result, "DXGI AcquireNextFrame");
        {
            const AcquiredFrame release{duplication.output_.Get()};
            if (stop.stop_requested()) break;
            duplication.pointer.update(duplication.output_.Get(), info);
            ComPtr<ID3D11Texture2D> texture;
            require(resource.As(&texture), "DXGI desktop texture");
            last = duplication.read(texture.Get(), onSize);
        }
        if (stop.stop_requested()) break;
        submit(source, last.get(), timestamp, false);
        lastSubmit = timestamp;
        nextCapture = std::max(nextCapture + interval, timestamp + interval - 750);
    }
}

DesktopCapture::~DesktopCapture() { stop(); }

bool DesktopCapture::start(webrtc_AdaptedVideoTrackSource* source, int maxFps,
                           std::function<void(QString)> onError, std::function<void(int, int)> onSize) {
    if (worker_.joinable() || !source || maxFps < 1 || maxFps > 240) return false;
    setMaxFps(maxFps);
    try {
        worker_ = std::jthread([this, source, onError = std::move(onError), onSize = std::move(onSize)](std::stop_token stop) {
            try {
                run(stop, source, onSize);
            } catch (const std::exception& error) {
                if (!stop.stop_requested() && onError) onError(QString::fromUtf8(error.what()));
            }
        });
    } catch (const std::system_error&) {
        return false;
    }
    return true;
}

void DesktopCapture::setMaxFps(int maxFps) {
    if (maxFps < 1 || maxFps > 240) return;
    {
        const std::scoped_lock lock(cadenceMutex_);
        maxFps_ = maxFps;
    }
    cadenceChanged_.notify_one();
}

void DesktopCapture::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

}  // namespace evp
