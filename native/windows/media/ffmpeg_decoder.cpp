#include "ffmpeg_decoder.h"

#include "video_surface.h"

#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace evp {
namespace {

using Microsoft::WRL::ComPtr;

std::shared_ptr<AVFrame> ownFrame(AVFrame* frame) {
    return {frame, [](AVFrame* owned) { av_frame_free(&owned); }};
}

VideoSurface::Color frameColor(const AVFrame& frame) {
    auto matrix = frame.width >= 1280 || frame.height >= 720
        ? VideoSurface::Matrix::Bt709 : VideoSurface::Matrix::Bt601;
    switch (frame.colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        matrix = VideoSurface::Matrix::Bt601;
        break;
    case AVCOL_SPC_BT709:
        matrix = VideoSurface::Matrix::Bt709;
        break;
    case AVCOL_SPC_BT2020_NCL:
        matrix = VideoSurface::Matrix::Bt2020;
        break;
    default:
        break;
    }
    return {matrix, frame.color_range == AVCOL_RANGE_JPEG || frame.format == AV_PIX_FMT_YUVJ420P};
}

AVPixelFormat selectFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_D3D11) return *format;
    }
    for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        const auto* descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *format;
    }
    return AV_PIX_FMT_NONE;
}

bool decoderSupportsD3D11(const AVCodec* codec) {
    for (int i = 0; const auto* config = avcodec_get_hw_config(codec, i); ++i) {
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA && config->pix_fmt == AV_PIX_FMT_D3D11
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
    }
    return false;
}

AVBufferRef* prepareD3D11(ComPtr<ID3D11Device> device) {
    if (!device) return nullptr;
    ComPtr<ID3D11VideoDevice> videoDevice;
    if (FAILED(device.As(&videoDevice))) return nullptr;
    // Confirm that the presentation device can sample both planes of an NV12 texture.
    D3D11_TEXTURE2D_DESC sample{};
    sample.Width = 64;
    sample.Height = 64;
    sample.MipLevels = 1;
    sample.ArraySize = 1;
    sample.Format = DXGI_FORMAT_NV12;
    sample.SampleDesc.Count = 1;
    sample.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&sample, nullptr, &texture))) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    for (const auto format : {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM}) {
        view.Format = format;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), &view, &srv))) return nullptr;
    }

    ComPtr<ID3D11DeviceContext> immediate;
    device->GetImmediateContext(&immediate);
    ComPtr<ID3D11Multithread> lock;
    if (FAILED(immediate.As(&lock))) return nullptr;

    auto* reference = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!reference) return nullptr;
    auto* context = reinterpret_cast<AVHWDeviceContext*>(reference->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
    d3d->device = device.Detach();
    d3d->device_context = immediate.Detach();
    d3d->video_device = videoDevice.Detach();
    d3d->lock = [](void* context) { static_cast<ID3D11Multithread*>(context)->Enter(); };
    d3d->unlock = [](void* context) { static_cast<ID3D11Multithread*>(context)->Leave(); };
    d3d->lock_ctx = lock.Detach();
    context->user_opaque = d3d->lock_ctx;
    context->free = [](AVHWDeviceContext* context) {
        static_cast<ID3D11Multithread*>(context->user_opaque)->Release();
    };
    if (av_hwdevice_ctx_init(reference) < 0) av_buffer_unref(&reference);
    return reference;
}

int swsMatrix(VideoSurface::Matrix matrix) {
    if (matrix == VideoSurface::Matrix::Bt709) return SWS_CS_ITU709;
    if (matrix == VideoSurface::Matrix::Bt2020) return SWS_CS_BT2020;
    return SWS_CS_ITU601;
}

}  // namespace

DecodedVideoFrame::DecodedVideoFrame(std::shared_ptr<AVFrame> frame,
                                     std::shared_ptr<std::atomic<std::uint64_t>> downloads)
    : frame_(std::move(frame)), downloads_(std::move(downloads)) {}

int DecodedVideoFrame::width() const { return frame_->width; }
int DecodedVideoFrame::height() const { return frame_->height; }
std::uint32_t DecodedVideoFrame::rtpTimestamp() const { return static_cast<std::uint32_t>(frame_->pts); }
bool DecodedVideoFrame::isHardware() const { return frame_->format == AV_PIX_FMT_D3D11; }

bool DecodedVideoFrame::submit(VideoSurface& surface, int rotation) const {
    const auto color = frameColor(*frame_);
    if (isHardware()) {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
        const auto slice = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(frame_->data[1]));
        return surface.submitNv12(width(), height(), texture, slice, frame_, color, rotation);
    }
    if (frame_->format == AV_PIX_FMT_BGRA) {
        return surface.submitBgra(width(), height(), frame_->data[0], frame_->linesize[0], rotation, frame_);
    }
    return surface.submitI420(width(), height(), frame_->data[0], frame_->linesize[0],
        frame_->data[1], frame_->linesize[1], frame_->data[2], frame_->linesize[2], rotation, frame_, color);
}

bool DecodedVideoFrame::copyI420(std::uint8_t* y, int strideY, std::uint8_t* u, int strideU,
                                 std::uint8_t* v, int strideV) const {
    const auto chromaWidth = (width() + 1) / 2;
    const auto chromaHeight = (height() + 1) / 2;
    if (!y || !u || !v || strideY < width() || strideU < chromaWidth || strideV < chromaWidth) return false;
    if (frame_->format == AV_PIX_FMT_YUV420P || frame_->format == AV_PIX_FMT_YUVJ420P) {
        av_image_copy_plane(y, strideY, frame_->data[0], frame_->linesize[0], width(), height());
        av_image_copy_plane(u, strideU, frame_->data[1], frame_->linesize[1], chromaWidth, chromaHeight);
        av_image_copy_plane(v, strideV, frame_->data[2], frame_->linesize[2], chromaWidth, chromaHeight);
        return true;
    }

    std::shared_ptr<AVFrame> downloaded;
    const auto* source = frame_.get();
    if (isHardware()) {
        downloaded = ownFrame(av_frame_alloc());
        if (!downloaded || av_hwframe_transfer_data(downloaded.get(), source, 0) < 0) return false;
        downloads_->fetch_add(1, std::memory_order_relaxed);
        source = downloaded.get();
    }
    // swscale requires padding beyond output planes. Convert into an AVFrame
    // before copying to buffers owned by WebRTC or another caller.
    auto planar = ownFrame(av_frame_alloc());
    if (!planar) return false;
    planar->format = AV_PIX_FMT_YUV420P;
    planar->width = width();
    planar->height = height();
    if (av_frame_get_buffer(planar.get(), 0) < 0) return false;
    const auto format = static_cast<AVPixelFormat>(source->format);
    const std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler(
        sws_getContext(width(), height(), format, width(), height(), AV_PIX_FMT_YUV420P,
                       SWS_BILINEAR, nullptr, nullptr, nullptr), sws_freeContext);
    if (!scaler) return false;
    const auto color = frameColor(*frame_);
    const auto* coefficients = sws_getCoefficients(swsMatrix(color.matrix));
    if (sws_setColorspaceDetails(scaler.get(), coefficients, format == AV_PIX_FMT_BGRA || color.fullRange,
                                  coefficients, color.fullRange, 0, 1 << 16, 1 << 16) < 0) return false;
    if (sws_scale(scaler.get(), source->data, source->linesize, 0, height(), planar->data, planar->linesize)
        != height()) return false;
    av_image_copy_plane(y, strideY, planar->data[0], planar->linesize[0], width(), height());
    av_image_copy_plane(u, strideU, planar->data[1], planar->linesize[1], chromaWidth, chromaHeight);
    av_image_copy_plane(v, strideV, planar->data[2], planar->linesize[2], chromaWidth, chromaHeight);
    return true;
}

struct FfmpegDecoder::State {
    explicit State(VideoSurface& output) : surface(&output), onFrame([&output](const DecodedVideoFrame& frame) {
        return frame.submit(output);
    }) {}

    State(ID3D11Device* suppliedDevice, std::function<bool(const DecodedVideoFrame&)> callback)
        : device(suppliedDevice), onFrame(std::move(callback)) {}

    ~State() {
        avcodec_free_context(&decoder);
        sws_freeContext(scaler);
    }

    bool fail(const char* action, int code) {
        char details[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(code, details, sizeof(details));
        error = std::string(action) + ": " + details;
        return false;
    }

    int openCodec(const AVCodec* codec, const AVCodecParameters& parameters, bool preferHardware) {
        decoder = avcodec_alloc_context3(codec);
        if (!decoder) return AVERROR(ENOMEM);
        auto result = avcodec_parameters_to_context(decoder, &parameters);
        if (result < 0) return result;
        decoder->thread_count = 4;
        decoder->thread_type = FF_THREAD_SLICE;
        if (preferHardware && decoderSupportsD3D11(codec)) {
            if (surface) device.Attach(surface->acquireD3D11Device());
            decoder->hw_device_ctx = prepareD3D11(device);
            if (decoder->hw_device_ctx) {
                decoder->get_format = selectFormat;
                // The renderer can retain one current and one pending decoder surface.
                decoder->extra_hw_frames = 3;
            }
        }
        return avcodec_open2(decoder, codec, nullptr);
    }

    std::shared_ptr<AVFrame> prepareCpu(const std::shared_ptr<AVFrame>& frame) {
        const auto format = static_cast<AVPixelFormat>(frame->format);
        const auto color = frameColor(*frame);
        if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_BGRA) return frame;

        auto converted = ownFrame(av_frame_alloc());
        if (!converted) {
            fail("Allocate software conversion frame", AVERROR(ENOMEM));
            return {};
        }
        converted->format = AV_PIX_FMT_BGRA;
        converted->width = frame->width;
        converted->height = frame->height;
        auto result = av_frame_get_buffer(converted.get(), 0);
        if (result >= 0) result = av_frame_copy_props(converted.get(), frame.get());
        if (result < 0) {
            fail("Allocate BGRA planes and properties", result);
            return {};
        }
        scaler = sws_getCachedContext(scaler, frame->width, frame->height, format, frame->width,
                                      frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) {
            fail("Initialize software pixel conversion", AVERROR(EINVAL));
            return {};
        }
        const auto* coefficients = sws_getCoefficients(swsMatrix(color.matrix));
        const auto colorspace = sws_setColorspaceDetails(scaler, coefficients, color.fullRange,
            coefficients, 1, 0, 1 << 16, 1 << 16);
        if (colorspace < 0) {
            fail("Set software conversion colorspace", colorspace);
            return {};
        }
        const auto rows = sws_scale(scaler, frame->data, frame->linesize, 0, frame->height,
                                    converted->data, converted->linesize);
        if (rows != frame->height) {
            fail("Convert frame to BGRA", rows < 0 ? rows : AVERROR(EIO));
            return {};
        }
        return converted;
    }

    bool submitFrame(const std::shared_ptr<AVFrame>& frame) {
        std::shared_ptr<AVFrame> prepared;
        if (frame->format == AV_PIX_FMT_D3D11) {
            ++stats.gpuFrames;
            auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
            D3D11_TEXTURE2D_DESC description{};
            if (texture) texture->GetDesc(&description);
            if (description.Format == DXGI_FORMAT_NV12) {
                prepared = frame;
            } else {
                auto downloaded = ownFrame(av_frame_alloc());
                if (!downloaded) return fail("Allocate hardware transfer frame", AVERROR(ENOMEM));
                auto result = av_hwframe_transfer_data(downloaded.get(), frame.get(), 0);
                if (result < 0) return fail("Transfer unsupported D3D11 format", result);
                result = av_frame_copy_props(downloaded.get(), frame.get());
                if (result < 0) return fail("Copy frame metadata", result);
                ++stats.hardwareDownloads;
                prepared = prepareCpu(downloaded);
            }
        } else {
            ++stats.cpuFrames;
            prepared = prepareCpu(frame);
        }
        if (!prepared || !onFrame || !onFrame(DecodedVideoFrame(std::move(prepared), downloads))) {
            if (error.empty()) error = "Output rejected the decoded frame";
            return false;
        }
        ++stats.submitted;
        return true;
    }

    bool receive() {
        auto frame = ownFrame(av_frame_alloc());
        if (!frame) return fail("Allocate decoder frame", AVERROR(ENOMEM));
        bool submitted = true;
        for (;;) {
            const auto result = avcodec_receive_frame(decoder, frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return submitted;
            if (result < 0) return fail("Receive decoded H.264 frame", result);
            submitted = submitFrame(frame) && submitted;
            frame = ownFrame(av_frame_alloc());
            if (!frame) return fail("Allocate decoder frame", AVERROR(ENOMEM));
        }
    }

    bool send(const AVPacket* packet) {
        error.clear();
        if (!decoder) return fail("Decode before opening the codec", AVERROR(EINVAL));
        bool submitted = true;
        auto result = avcodec_send_packet(decoder, packet);
        if (result == AVERROR(EAGAIN)) {
            submitted = receive();
            result = avcodec_send_packet(decoder, packet);
        }
        if (!packet && result == AVERROR_EOF) return submitted;
        if (result < 0) return fail("Send H.264 packet to decoder", result);
        return receive() && submitted;
    }

    VideoSurface* surface = nullptr;
    ComPtr<ID3D11Device> device;
    std::function<bool(const DecodedVideoFrame&)> onFrame;
    AVCodecContext* decoder = nullptr;
    SwsContext* scaler = nullptr;
    Stats stats;
    std::shared_ptr<std::atomic<std::uint64_t>> downloads;
    std::string error;
};

FfmpegDecoder::FfmpegDecoder(VideoSurface& surface) : state_(std::make_unique<State>(surface)) {}
FfmpegDecoder::FfmpegDecoder(ID3D11Device* device, std::function<bool(const DecodedVideoFrame&)> onFrame)
    : state_(std::make_unique<State>(device, std::move(onFrame))) {}
FfmpegDecoder::~FfmpegDecoder() = default;

bool FfmpegDecoder::open(const AVCodecParameters& parameters, bool preferHardware) {
    avcodec_free_context(&state_->decoder);
    state_->error.clear();
    state_->stats = {};
    state_->downloads = std::make_shared<std::atomic<std::uint64_t>>(0);
    if (parameters.codec_type != AVMEDIA_TYPE_VIDEO || parameters.codec_id != AV_CODEC_ID_H264) {
        return state_->fail("Open H.264 video decoder", AVERROR(EINVAL));
    }
    const auto* codec = avcodec_find_decoder(parameters.codec_id);
    if (!codec) return state_->fail("Find H.264 decoder", AVERROR_DECODER_NOT_FOUND);

    auto result = state_->openCodec(codec, parameters, preferHardware);
    if (result < 0 && state_->decoder && state_->decoder->hw_device_ctx) {
        avcodec_free_context(&state_->decoder);
        result = state_->openCodec(codec, parameters, false);
    }
    if (result >= 0) return true;
    avcodec_free_context(&state_->decoder);
    return state_->fail("Open H.264 video decoder", result);
}

bool FfmpegDecoder::decode(const AVPacket& packet) { return state_->send(&packet); }
bool FfmpegDecoder::openH264(bool preferHardware) {
    AVCodecParameters parameters{};
    parameters.codec_type = AVMEDIA_TYPE_VIDEO;
    parameters.codec_id = AV_CODEC_ID_H264;
    if (!open(parameters, preferHardware)) return false;
    state_->decoder->pkt_timebase = {1, 90000};
    return true;
}

bool FfmpegDecoder::decodeAnnexB(std::span<const std::uint8_t> bytes, std::uint32_t rtpTimestamp) {
    if (bytes.empty() || bytes.size() > (std::numeric_limits<int>::max)()) {
        return state_->fail("Validate H.264 access unit", AVERROR(EINVAL));
    }
    const auto release = [](AVPacket* packet) { av_packet_free(&packet); };
    const std::unique_ptr<AVPacket, decltype(release)> packet(av_packet_alloc(), release);
    if (!packet) return state_->fail("Allocate RTP decode packet", AVERROR(ENOMEM));
    // FFmpeg reads zero padding past the payload while parsing H.264.
    const auto result = av_new_packet(packet.get(), static_cast<int>(bytes.size()));
    if (result < 0) return state_->fail("Allocate padded RTP payload", result);
    std::memcpy(packet->data, bytes.data(), bytes.size());
    packet->pts = rtpTimestamp;
    return decode(*packet);
}

bool FfmpegDecoder::flush() { return state_->send(nullptr); }
bool FfmpegDecoder::hasHardwareDevice() const { return state_->decoder && state_->decoder->hw_device_ctx; }
FfmpegDecoder::Stats FfmpegDecoder::stats() const {
    auto stats = state_->stats;
    if (state_->downloads) stats.hardwareDownloads += state_->downloads->load(std::memory_order_relaxed);
    return stats;
}
std::string FfmpegDecoder::lastError() const { return state_->error; }

}  // namespace evp
