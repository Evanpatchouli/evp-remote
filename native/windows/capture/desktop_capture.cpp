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
#include <cmath>
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
        image = {};
        imageChecked = false;
        if (frame.PointerShapeBufferSize > 1024 * 1024) {
            pixels.clear();
            return;
        }
        pixels.resize(frame.PointerShapeBufferSize);
        UINT required = 0;
        if (FAILED(output->GetFramePointerShape(static_cast<UINT>(pixels.size()), pixels.data(), &required, &shape)) ||
            required > pixels.size()) {
            pixels.clear();
            return;
        }
        pixels.resize(required);
    }

    bool valid() const {
        const bool monochrome = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const bool color = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
            shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
        return (color || monochrome) && shape.Width && shape.Height && shape.Pitch &&
            shape.Height <= pixels.size() / shape.Pitch &&
            (monochrome ? !(shape.Height & 1) && (shape.Width - 1) / 8u < shape.Pitch
                        : shape.Width <= shape.Pitch / 4u);
    }

    int height() const {
        return static_cast<int>(shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? shape.Height / 2 : shape.Height);
    }
    QPoint topLeft() const { return {position.Position.x, position.Position.y}; }
    QRect bounds(QSize desktop) const {
        if (!valid()) return {};
        return QRect(topLeft(), QSize(static_cast<int>(shape.Width), height())).intersected(QRect(QPoint(), desktop));
    }

    bool visible() const { return position.Visible && valid(); }

    const QImage& separateImage() {
        if (imageChecked) return image;
        imageChecked = true;
        if (!valid() || shape.Width > maxCursorEdge || height() > maxCursorEdge ||
            cursorHeaderBytes + 4 * shape.Width * height() > maxCursorPacket ||
            shape.HotSpot.x < 0 || unsigned(shape.HotSpot.x) >= shape.Width ||
            shape.HotSpot.y < 0 || shape.HotSpot.y >= height()) return image;
        QImage candidate(static_cast<int>(shape.Width), height(), QImage::Format_ARGB32_Premultiplied);
        if (candidate.isNull()) return image;
        for (int row = 0; row < candidate.height(); ++row) {
            auto* dst = reinterpret_cast<QRgb*>(candidate.scanLine(row));
            const auto* src = pixels.data() + static_cast<size_t>(row) * shape.Pitch;
            for (int column = 0; column < candidate.width(); ++column) {
                if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    const auto bit = 0x80 >> (column & 7);
                    const bool andMask = src[column / 8] & bit;
                    const bool xorMask = src[static_cast<size_t>(height()) * shape.Pitch + column / 8] & bit;
                    if (andMask && xorMask) return image;
                    const int level = xorMask ? 255 : 0;
                    dst[column] = qRgba(level, level, level, andMask ? 0 : 255);
                } else {
                    const auto* pixel = src + column * 4;
                    if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                        if (pixel[3] && (pixel[0] || pixel[1] || pixel[2])) return image;
                        dst[column] = pixel[3] ? 0 : qRgb(pixel[2], pixel[1], pixel[0]);
                    } else {
                        dst[column] = qPremultiply(qRgba(pixel[2], pixel[1], pixel[0], pixel[3]));
                    }
                }
            }
        }
        image = std::move(candidate);
        return image;
    }

    void draw(std::uint8_t* output, int pitch, const QRect& clipped) const {
        const bool monochrome = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
            for (int x = clipped.left(); x <= clipped.right(); ++x) {
                const int row = y - position.Position.y;
                const int column = x - position.Position.x;
                auto* dst = output + static_cast<size_t>(y) * pitch + x * 4;
                const auto* src = pixels.data() + static_cast<size_t>(row) * shape.Pitch;
                if (monochrome) {
                    const auto bit = 0x80 >> (column & 7);
                    const auto andMask = src[column / 8] & bit ? 255 : 0;
                    const auto xorMask = src[static_cast<size_t>(height()) * shape.Pitch + column / 8] & bit ? 255 : 0;
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
    QImage image;
    bool imageChecked = false;
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
                desktop = QSize(description.DesktopCoordinates.right - description.DesktopCoordinates.left,
                                description.DesktopCoordinates.bottom - description.DesktopCoordinates.top);
                return;
            }
        }
        throw std::runtime_error("No DXGI primary output");
    }

    bool rotated() const {
        return mode.Rotation == DXGI_MODE_ROTATION_ROTATE90 || mode.Rotation == DXGI_MODE_ROTATION_ROTATE270;
    }

    CursorSnapshot cursor() {
        CursorSnapshot state;
        state.desktop = width && height ? QSize(width, height) : desktop;
        CURSORINFO info{sizeof(CURSORINFO)};
        if (GetCursorInfo(&info) && (!(info.flags & CURSOR_SHOWING) || (info.flags & CURSOR_SUPPRESSED))) {
            state.mode = CursorMode::Hidden;
            return state;
        }
        if (!pointer.position.Visible) return state;
        state.mode = CursorMode::Composited;
        const auto& image = pointer.separateImage();
        const QPoint hotSpot(pointer.shape.HotSpot.x, pointer.shape.HotSpot.y);
        if (!image.isNull() && QRect(QPoint(), state.desktop).contains(pointer.topLeft() + hotSpot)) {
            state.mode = CursorMode::Separate;
            state.topLeft = pointer.topLeft();
            state.hotSpot = hotSpot;
            state.image = image;
        }
        return state;
    }

    FrameBuffer read(ID3D11Texture2D* texture, const std::function<void(int, int)>& onSize, bool composite) {
        if (texture) {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            prepare(description, onSize);
        }
        return convert(texture, composite);
    }

    void prepare(D3D11_TEXTURE2D_DESC description, const std::function<void(int, int)>& onSize) {
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
    }

    FrameBuffer convert(ID3D11Texture2D* texture, bool composite) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(readbackTime.measure([&] {
            if (texture) context->CopyResource(staging.Get(), texture);
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
        // Preserve the clean staging image for a cursor mode change on a static desktop.
        const auto bounds = composite ? pointer.bounds(QSize(width, height)) : QRect{};
        if (!bounds.isEmpty()) {
            if (pixels == mapped.pData) saveCursorArea(pixels, pitch, bounds, false);
            pointer.draw(pixels, pitch, bounds);
        }
        const auto converted = conversionTime.measure([&] {
            return sws_scale(conversion.get(), src, srcPitch, 0, height, dst, dstPitch);
        });
        if (!bounds.isEmpty() && pixels == mapped.pData) saveCursorArea(pixels, pitch, bounds, true);
        if (converted != (height & ~1)) {
            throw std::runtime_error("FFmpeg BGRA conversion failed");
        }
        return buffer;
    }

    void saveCursorArea(std::uint8_t* pixels, int pitch, const QRect& bounds, bool restore) {
        const auto rowBytes = static_cast<size_t>(bounds.width()) * 4;
        if (!restore) cursorArea.resize(rowBytes * bounds.height());
        for (int row = 0; row < bounds.height(); ++row) {
            auto* screen = pixels + static_cast<size_t>(bounds.top() + row) * pitch + bounds.left() * 4;
            auto* saved = cursorArea.data() + row * rowBytes;
            if (restore) std::memcpy(screen, saved, rowBytes);
            else std::memcpy(saved, screen, rowBytes);
        }
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
    QSize desktop;
    Pointer pointer;
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> conversion{nullptr, sws_freeContext};
    std::vector<std::uint8_t> oriented;
    std::vector<std::uint8_t> cursorArea;
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
                         const std::function<void(int, int)>& onSize,
                         const std::function<void(quint64, CursorSnapshot)>& onCursor) {
    Duplication duplication;
    FrameBuffer last;
    bool lastComposite = false;
    quint64 lastCursorEpoch = 0;
    CursorSnapshot lastCursor;
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
        if (result != DXGI_ERROR_WAIT_TIMEOUT) require(result, "DXGI AcquireNextFrame");
        bool updated = false;
        const auto epoch = onCursor ? cursorEpoch_.load() : 0;
        CursorSnapshot cursor;
        const auto capture = [&](ID3D11Texture2D* texture, bool desktopUpdated) {
            if (epoch) {
                cursor = duplication.cursor();
                cursor.appliedMove = appliedCursorMove(epoch, cursor);
            }
            const bool composite = (!epoch || cursor.mode == CursorMode::Composited) && duplication.pointer.visible();
            if ((texture && (!last || desktopUpdated || composite)) || (last && composite != lastComposite)) {
                last = duplication.read(texture, onSize, composite);
                lastComposite = composite;
                updated = true;
            }
        };
        if (result == DXGI_ERROR_WAIT_TIMEOUT) {
            if (last) capture(nullptr, false);
        } else {
            const AcquiredFrame release{duplication.output_.Get()};
            if (stop.stop_requested()) break;
            duplication.pointer.update(duplication.output_.Get(), info);
            ComPtr<ID3D11Texture2D> texture;
            require(resource.As(&texture), "DXGI desktop texture");
            capture(texture.Get(), !epoch || info.LastPresentTime.QuadPart != 0);
        }
        if (stop.stop_requested()) break;
        if (epoch && last && (lastCursorEpoch != epoch || lastCursor != cursor)) {
            lastCursor = cursor;
            onCursor(epoch, std::move(cursor));
        }
        lastCursorEpoch = epoch;
        if (!updated && (!last || timestamp - lastSubmit < 250'000)) {
            if (epoch && result != DXGI_ERROR_WAIT_TIMEOUT)
                nextCapture = timestamp + 8'333;
            continue;
        }
        submit(source, last.get(), timestamp, !updated);
        lastSubmit = timestamp;
        nextCapture = std::max(nextCapture + interval, timestamp + interval - 750);
    }
}

DesktopCapture::~DesktopCapture() { stop(); }

bool DesktopCapture::start(webrtc_AdaptedVideoTrackSource* source, int maxFps,
                           std::function<void(QString)> onError, std::function<void(int, int)> onSize,
                           std::function<void(quint64, CursorSnapshot)> onCursor) {
    if (worker_.joinable() || !source || maxFps < 1 || maxFps > 240) return false;
    setMaxFps(maxFps);
    try {
        worker_ = std::jthread([this, source, onError = std::move(onError), onSize = std::move(onSize),
                                onCursor = std::move(onCursor)](std::stop_token stop) {
            try {
                run(stop, source, onSize, onCursor);
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

void DesktopCapture::setCursorEpoch(quint64 epoch) {
    cursorEpoch_ = epoch;
}

void DesktopCapture::noteCursorMove(quint64 epoch, quint32 serial, QPointF position) {
    const std::lock_guard lock(cursorMoveMutex_);
    cursorMove_ = {epoch, serial, position, false};
}

quint32 DesktopCapture::appliedCursorMove(quint64 epoch, const CursorSnapshot& state) {
    if (state.mode != CursorMode::Separate) return 0;
    const std::lock_guard lock(cursorMoveMutex_);
    if (cursorMove_.epoch != epoch || !cursorMove_.serial) return 0;
    const auto hot = state.topLeft + state.hotSpot;
    const auto& point = cursorMove_.position;
    if (std::abs(hot.x() - point.x() * (state.desktop.width() - 1)) <= 2 &&
        std::abs(hot.y() - point.y() * (state.desktop.height() - 1)) <= 2) {
        cursorMove_.matched = true;
        return cursorMove_.serial;
    }
    if (cursorMove_.matched) cursorMove_.serial = 0;
    return 0;
}

void DesktopCapture::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
    cursorEpoch_ = 0;
    const std::lock_guard lock(cursorMoveMutex_);
    cursorMove_ = {};
}

}  // namespace evp
