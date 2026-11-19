#include "webrtc_ffmpeg_encoder.h"

#include <webrtc_c/api/video/encoded_image.h>
#include <webrtc_c/api/video/i420_buffer.h>
#include <webrtc_c/api/video/video_frame.h>
#include <webrtc_c/api/video_codecs/video_encoder_factory.h>
#include <webrtc_c/libyuv.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <memory>
#include <string_view>

namespace evp {
namespace {

struct Backend {
    const char* codec;
    const char* name;
    const char* vendor;
    AVPixelFormat hardwareFormat;
};

constexpr Backend nvenc{"h264_nvenc", "FFmpeg NVENC (D3D11)", "0x10de", AV_PIX_FMT_D3D11};
constexpr Backend amf{"h264_amf", "FFmpeg AMF (D3D11)", "0x1002", AV_PIX_FMT_D3D11};
constexpr Backend qsv{"h264_qsv", "FFmpeg Quick Sync (D3D11)", "0x8086", AV_PIX_FMT_QSV};
constexpr Backend x264{"libx264", "FFmpeg x264 (CPU)", nullptr, AV_PIX_FMT_NONE};
constexpr std::array backends{&nvenc, &amf, &qsv, &x264};
constexpr std::size_t maxPendingFrames = 8;

struct Callbacks {
    std::function<void(QString)> onError;
    std::function<void(QString)> onBackend;
};

struct FreeCodec { void operator()(AVCodecContext* context) const { avcodec_free_context(&context); } };
struct FreeFilter { void operator()(AVBSFContext* filter) const { av_bsf_free(&filter); } };
struct FreeFrame { void operator()(AVFrame* frame) const { av_frame_free(&frame); } };
struct FreePacket { void operator()(AVPacket* packet) const { av_packet_free(&packet); } };
struct FreeBuffer { void operator()(AVBufferRef* buffer) const { av_buffer_unref(&buffer); } };

bool configureHardware(AVCodecContext& codec, const Backend& backend) {
    AVDictionary* options = nullptr;
    av_dict_set(&options, "vendor_id", backend.vendor, 0);
    AVBufferRef* device = nullptr;
    const auto result = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, options, 0);
    av_dict_free(&options);
    std::unique_ptr<AVBufferRef, FreeBuffer> d3d11(device);
    if (result < 0) return false;
    if (backend.hardwareFormat == AV_PIX_FMT_QSV) {
        if (av_hwdevice_ctx_create_derived(&codec.hw_device_ctx, AV_HWDEVICE_TYPE_QSV, d3d11.get(), 0) < 0) return false;
    } else {
        codec.hw_device_ctx = av_buffer_ref(d3d11.get());
    }
    if (!codec.hw_device_ctx) return false;

    codec.hw_frames_ctx = av_hwframe_ctx_alloc(codec.hw_device_ctx);
    if (!codec.hw_frames_ctx) return false;
    auto* frames = reinterpret_cast<AVHWFramesContext*>(codec.hw_frames_ctx->data);
    frames->format = backend.hardwareFormat;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = codec.width;
    frames->height = codec.height;
    // D3D11 allocates independent NV12 textures when no array size is supplied.
    // This works with drivers that reject NV12 texture arrays for encoding.
    if (backend.hardwareFormat == AV_PIX_FMT_QSV) {
        frames->initial_pool_size = static_cast<int>(maxPendingFrames);
    }
    return av_hwframe_ctx_init(codec.hw_frames_ctx) >= 0;
}

void setOptions(AVDictionary** options, const Backend& backend) {
    if (&backend == &nvenc) {
        av_dict_set(options, "forced-idr", "1", 0);
        av_dict_set(options, "profile", "baseline", 0);
        av_dict_set(options, "preset", "p3", 0);
        av_dict_set(options, "tune", "ull", 0);
        av_dict_set(options, "rc", "cbr", 0);
        av_dict_set(options, "delay", "0", 0);
        av_dict_set(options, "zerolatency", "1", 0);
    } else if (&backend == &amf) {
        av_dict_set(options, "forced_idr", "1", 0);
        av_dict_set(options, "profile", "constrained_baseline", 0);
        av_dict_set(options, "usage", "ultralowlatency", 0);
        av_dict_set(options, "quality", "speed", 0);
        av_dict_set(options, "rc", "cbr", 0);
        av_dict_set(options, "async_depth", "1", 0);
        av_dict_set(options, "preanalysis", "0", 0);
    } else if (&backend == &qsv) {
        av_dict_set(options, "forced_idr", "1", 0);
        av_dict_set(options, "profile", "baseline", 0);
        av_dict_set(options, "scenario", "displayremoting", 0);
        av_dict_set(options, "preset", "veryfast", 0);
        av_dict_set(options, "async_depth", "1", 0);
        av_dict_set(options, "look_ahead", "0", 0);
        av_dict_set(options, "repeat_pps", "1", 0);
    } else {
        av_dict_set(options, "forced-idr", "1", 0);
        av_dict_set(options, "profile", "baseline", 0);
        av_dict_set(options, "preset", "ultrafast", 0);
        av_dict_set(options, "tune", "zerolatency", 0);
        av_dict_set(options, "x264-params", "annexb=1:repeat-headers=1:scenecut=0", 0);
    }
}

struct Encoder {
    struct Stamp { std::int64_t pts; std::uint32_t rtpTimestamp; };

    bool open(const Backend& backend) {
        const auto* implementation = avcodec_find_encoder_by_name(backend.codec);
        if (!implementation) return false;
        std::unique_ptr<AVCodecContext, FreeCodec> candidate(avcodec_alloc_context3(implementation));
        if (!candidate) return false;
        candidate->width = width;
        candidate->height = height;
        candidate->framerate = {frameRate, 1};
        candidate->time_base = {1, 90'000};
        candidate->gop_size = frameRate * 2;
        candidate->max_b_frames = 0;
        candidate->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
        candidate->flags |= AV_CODEC_FLAG_LOW_DELAY;
        candidate->bit_rate = bitRate;
        candidate->rc_max_rate = bitRate;
        candidate->rc_buffer_size = std::max(10'000, bitRate / 5);
        candidate->color_range = AVCOL_RANGE_MPEG;
        candidate->color_primaries = AVCOL_PRI_BT709;
        candidate->color_trc = AVCOL_TRC_BT709;
        candidate->colorspace = AVCOL_SPC_BT709;
        candidate->thread_count = threadCount;
        candidate->pix_fmt = backend.vendor ? backend.hardwareFormat : AV_PIX_FMT_YUV420P;
        if (backend.vendor && !configureHardware(*candidate, backend)) return false;

        AVDictionary* options = nullptr;
        setOptions(&options, backend);
        const auto result = avcodec_open2(candidate.get(), implementation, &options);
        const bool usedAllOptions = av_dict_count(options) == 0;
        av_dict_free(&options);
        if (result < 0 || !usedAllOptions) return false;

        AVBSFContext* filter = nullptr;
        const auto* annexB = av_bsf_get_by_name("h264_mp4toannexb");
        if (!annexB || av_bsf_alloc(annexB, &filter) < 0) return false;
        std::unique_ptr<AVBSFContext, FreeFilter> candidateFilter(filter);
        filter->time_base_in = candidate->time_base;
        if (avcodec_parameters_from_context(filter->par_in, candidate.get()) < 0 || av_bsf_init(filter) < 0) return false;

        std::unique_ptr<AVFrame, FreeFrame> input(av_frame_alloc());
        if (!input || !uploaded || !packet) return false;
        input->format = backend.vendor ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        input->width = width;
        input->height = height;
        input->color_range = candidate->color_range;
        input->color_primaries = candidate->color_primaries;
        input->color_trc = candidate->color_trc;
        input->colorspace = candidate->colorspace;
        if (av_frame_get_buffer(input.get(), 32) < 0) return false;

        codec = std::move(candidate);
        filter_ = std::move(candidateFilter);
        software = std::move(input);
        active = &backend;
        pending.clear();
        firstFrame = true;
        if (callbacks.onBackend) callbacks.onBackend(QString::fromLatin1(backend.name));
        return true;
    }

    void release() {
        av_frame_unref(uploaded.get());
        av_packet_unref(packet.get());
        filter_.reset();
        software.reset();
        codec.reset();
        active = nullptr;
        pending.clear();
    }

    void fail() {
        if (reportedError) return;
        reportedError = true;
        if (callbacks.onError) callbacks.onError(QStringLiteral("FFmpeg H.264 encoding failed"));
    }

    bool encode(const webrtc_VideoFrame* input, bool keyframe, int& emitted) {
        if (!drain(emitted) || pending.size() >= maxPendingFrames) return false;
        auto* buffer = webrtc_VideoFrame_video_frame_buffer(input);
        if (!buffer) return false;
        const std::unique_ptr<webrtc_VideoFrameBuffer, decltype(&webrtc_VideoFrameBuffer_Release)> source(
            webrtc_VideoFrameBuffer_refcounted_get(buffer), webrtc_VideoFrameBuffer_Release);
        auto* retained = webrtc_VideoFrameBuffer_ToI420(source.get());
        if (!retained || av_frame_make_writable(software.get()) < 0) {
            if (retained) webrtc_I420Buffer_Release(webrtc_I420Buffer_refcounted_get(retained));
            return false;
        }
        const std::unique_ptr<webrtc_I420Buffer, decltype(&webrtc_I420Buffer_Release)> i420(
            webrtc_I420Buffer_refcounted_get(retained), webrtc_I420Buffer_Release);
        const auto* y = webrtc_I420Buffer_MutableDataY(i420.get());
        const auto* u = webrtc_I420Buffer_MutableDataU(i420.get());
        const auto* v = webrtc_I420Buffer_MutableDataV(i420.get());
        const auto yStride = webrtc_I420Buffer_StrideY(i420.get());
        const auto uStride = webrtc_I420Buffer_StrideU(i420.get());
        const auto vStride = webrtc_I420Buffer_StrideV(i420.get());
        AVFrame* frame = software.get();
        if (codec->hw_frames_ctx) {
            if (libyuv_I420ToNV12(y, yStride, u, uStride, v, vStride,
                                  frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], width, height) != 0 ||
                av_hwframe_get_buffer(codec->hw_frames_ctx, uploaded.get(), 0) < 0 ||
                av_hwframe_transfer_data(uploaded.get(), frame, 0) < 0 || av_frame_copy_props(uploaded.get(), frame) < 0) {
                av_frame_unref(uploaded.get());
                return false;
            }
            frame = uploaded.get();
        } else if (libyuv_I420Copy(y, yStride, u, uStride, v, vStride,
                                  frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                  frame->data[2], frame->linesize[2], width, height) != 0) {
            return false;
        }

        const auto timestamp = webrtc_VideoFrame_timestamp_us(input);
        frame->pts = std::max(lastPts + 1, av_rescale_q(timestamp, AVRational{1, 1'000'000}, codec->time_base));
        lastPts = frame->pts;
        frame->pict_type = firstFrame || keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        firstFrame = false;
        const auto sent = avcodec_send_frame(codec.get(), frame);
        av_frame_unref(uploaded.get());
        if (sent < 0) return false;
        pending.push_back({lastPts, webrtc_VideoFrame_timestamp_rtp(input)});
        return drain(emitted);
    }

    bool drain(int& emitted) {
        for (;;) {
            const auto received = avcodec_receive_packet(codec.get(), packet.get());
            if (received == AVERROR(EAGAIN)) return true;
            if (received < 0) return false;
            if (av_bsf_send_packet(filter_.get(), packet.get()) < 0) {
                av_packet_unref(packet.get());
                return false;
            }
            while (true) {
                const auto filtered = av_bsf_receive_packet(filter_.get(), packet.get());
                if (filtered == AVERROR(EAGAIN)) break;
                if (filtered < 0) return false;
                const auto match = std::find_if(pending.begin(), pending.end(), [this](const Stamp& stamp) {
                    return stamp.pts == packet->pts;
                });
                const bool published = match != pending.end() && publish(match->rtpTimestamp);
                if (match != pending.end()) pending.erase(match);
                av_packet_unref(packet.get());
                if (!published) return false;
                ++emitted;
            }
        }
    }

    bool publish(std::uint32_t timestamp) const {
        const auto* bytes = packet->data;
        if (packet->size < 5 || bytes[0] != 0 || bytes[1] != 0 ||
            (bytes[2] != 1 && (bytes[2] != 0 || bytes[3] != 1))) return false;
        auto* retained = webrtc_EncodedImageBuffer_Create_from_data(bytes, packet->size);
        if (!retained) return false;
        const std::unique_ptr<webrtc_EncodedImageBuffer, decltype(&webrtc_EncodedImageBuffer_Release)> buffer(
            webrtc_EncodedImageBuffer_refcounted_get(retained), webrtc_EncodedImageBuffer_Release);
        const std::unique_ptr<webrtc_EncodedImage_unique, decltype(&webrtc_EncodedImage_unique_delete)> image(
            webrtc_EncodedImage_new(), webrtc_EncodedImage_unique_delete);
        const std::unique_ptr<webrtc_CodecSpecificInfo_unique, decltype(&webrtc_CodecSpecificInfo_unique_delete)> info(
            webrtc_CodecSpecificInfo_new(), webrtc_CodecSpecificInfo_unique_delete);
        if (!image || !info) return false;
        const bool keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        auto* output = webrtc_EncodedImage_unique_get(image.get());
        webrtc_EncodedImage_set_encoded_data(output, buffer.get());
        webrtc_EncodedImage_set_encoded_width(output, width);
        webrtc_EncodedImage_set_encoded_height(output, height);
        webrtc_EncodedImage_set_rtp_timestamp(output, timestamp);
        webrtc_EncodedImage_set_frame_type(output, keyframe ? webrtc_VideoFrameType_Key : webrtc_VideoFrameType_Delta);
        webrtc_EncodedImage_set_qp(output, -1);
        auto* details = webrtc_CodecSpecificInfo_unique_get(info.get());
        webrtc_CodecSpecificInfo_set_codec_type(details, webrtc_VideoCodecType_H264);
        webrtc_CodecSpecificInfo_set_end_of_picture(details, true);
        webrtc_CodecSpecificInfo_set_h264_packetization_mode(details, webrtc_H264PacketizationMode_NonInterleaved);
        webrtc_CodecSpecificInfo_set_h264_temporal_idx(details, 255);
        webrtc_CodecSpecificInfo_set_h264_idr_frame(details, keyframe);
        const std::unique_ptr<webrtc_VideoEncoder_EncodedImageCallback_Result_unique,
                              decltype(&webrtc_VideoEncoder_EncodedImageCallback_Result_unique_delete)> result(
            webrtc_VideoEncoder_EncodedImageCallback_OnEncodedImage(callback, output, details),
            webrtc_VideoEncoder_EncodedImageCallback_Result_unique_delete);
        // A transport-side rejection does not make the codec unusable.
        return true;
    }

    Callbacks callbacks;
    const Backend* active = nullptr;
    std::unique_ptr<AVCodecContext, FreeCodec> codec;
    std::unique_ptr<AVBSFContext, FreeFilter> filter_;
    std::unique_ptr<AVFrame, FreeFrame> software;
    std::unique_ptr<AVFrame, FreeFrame> uploaded{av_frame_alloc()};
    std::unique_ptr<AVPacket, FreePacket> packet{av_packet_alloc()};
    webrtc_VideoEncoder_EncodedImageCallback* callback = nullptr;
    std::deque<Stamp> pending;
    std::int64_t lastPts = -1;
    int width = 0;
    int height = 0;
    int threadCount = 1;
    int frameRate = 60;
    int bitRate = 1'000'000;
    bool firstFrame = true;
    bool paused = false;
    bool reportedError = false;
};

const webrtc_VideoEncoder_cbs encoderCallbacks{
    [](const webrtc_VideoCodec* config, const webrtc_VideoEncoder_Settings* settings, void* data) -> std::int32_t {
        if (webrtc_VideoCodec_codec_type(config) != webrtc_VideoCodecType_H264 ||
            webrtc_VideoCodec_number_of_simulcast_streams(config) > 1) return webrtc_VideoCodecStatus_ErrParameter;
        auto& encoder = *static_cast<Encoder*>(data);
        if (!encoder.uploaded || !encoder.packet) return webrtc_VideoCodecStatus_Memory;
        encoder.release();
        encoder.reportedError = false;
        encoder.paused = false;
        encoder.width = webrtc_VideoCodec_width(config);
        encoder.height = webrtc_VideoCodec_height(config);
        if (encoder.width < 2 || encoder.height < 2 || ((encoder.width | encoder.height) & 1)) {
            return webrtc_VideoCodecStatus_ErrParameter;
        }
        encoder.frameRate = static_cast<int>(std::clamp(webrtc_VideoCodec_max_framerate(config), 1u, 240u));
        encoder.bitRate = static_cast<int>(std::clamp(webrtc_VideoCodec_start_bitrate_kbps(config), 100u, 200'000u) * 1000);
        encoder.threadCount = std::clamp(webrtc_VideoEncoder_Settings_number_of_cores(settings), 1, 4);
        for (const auto* backend : backends) {
            if (encoder.open(*backend)) return webrtc_VideoCodecStatus_Ok;
        }
        encoder.fail();
        return webrtc_VideoCodecStatus_EncoderFailure;
    },
    [](const webrtc_VideoFrame* frame, const webrtc_VideoFrameType_vector* types, void* data) -> std::int32_t {
        auto& encoder = *static_cast<Encoder*>(data);
        if (!encoder.codec || !encoder.callback) return webrtc_VideoCodecStatus_Uninitialized;
        if (encoder.width != webrtc_VideoFrame_width(frame) || encoder.height != webrtc_VideoFrame_height(frame)) {
            return webrtc_VideoCodecStatus_ErrParameter;
        }
        const int type = types && webrtc_VideoFrameType_vector_size(types) > 0
            ? webrtc_VideoFrameType_value(webrtc_VideoFrameType_vector_get_const(types, 0)) : webrtc_VideoFrameType_Delta;
        if (encoder.paused || type == webrtc_VideoFrameType_Empty) return webrtc_VideoCodecStatus_NoOutput;
        int emitted = 0;
        if (!encoder.encode(frame, type == webrtc_VideoFrameType_Key, emitted)) {
            const bool hardware = encoder.active && encoder.active->vendor;
            encoder.release();
            if (!hardware || !encoder.open(x264) || !encoder.encode(frame, true, emitted)) {
                encoder.release();
                encoder.fail();
                return webrtc_VideoCodecStatus_EncoderFailure;
            }
        }
        return emitted ? webrtc_VideoCodecStatus_Ok : webrtc_VideoCodecStatus_NoOutput;
    },
    [](webrtc_VideoEncoder_EncodedImageCallback* callback, void* data) -> std::int32_t {
        static_cast<Encoder*>(data)->callback = callback;
        return webrtc_VideoCodecStatus_Ok;
    },
    [](void* data) -> std::int32_t {
        auto& encoder = *static_cast<Encoder*>(data);
        if (encoder.uploaded && encoder.packet) encoder.release();
        return webrtc_VideoCodecStatus_Ok;
    },
    [](const webrtc_VideoEncoder_RateControlParameters* parameters, void* data) {
        auto& encoder = *static_cast<Encoder*>(data);
        const auto rate = webrtc_VideoEncoder_RateControlParameters_bitrate_sum_bps(parameters);
        if (!rate) {
            encoder.paused = true;
            return;
        }
        if (encoder.paused) encoder.firstFrame = true;
        encoder.paused = false;
        encoder.bitRate = static_cast<int>(std::min(rate, 200'000'000u));
        if (!encoder.codec) return;
        encoder.codec->bit_rate = encoder.bitRate;
        encoder.codec->rc_max_rate = encoder.bitRate;
        encoder.codec->rc_buffer_size = std::max(10'000, encoder.bitRate / 5);
        const auto fps = webrtc_VideoEncoder_RateControlParameters_framerate_fps(parameters);
        if (std::isfinite(fps) && fps >= 1 && fps <= 240) {
            encoder.codec->framerate.num = static_cast<int>(std::lround(fps));
        }
    },
    [](void* data) {
        const auto& encoder = *static_cast<Encoder*>(data);
        auto* result = webrtc_VideoEncoder_EncoderInfo_new();
        auto* info = webrtc_VideoEncoder_EncoderInfo_unique_get(result);
        const auto* backend = encoder.active;
        const std::unique_ptr<std_string_unique, decltype(&std_string_unique_delete)> name(
            std_string_new_from_cstr(backend ? backend->name : "FFmpeg H.264"), std_string_unique_delete);
        webrtc_VideoEncoder_EncoderInfo_set_implementation_name(info, std_string_unique_get(name.get()));
        webrtc_VideoEncoder_EncoderInfo_set_requested_resolution_alignment(info, 2);
        webrtc_VideoEncoder_EncoderInfo_set_is_hardware_accelerated(info, backend && backend->vendor);
        return result;
    },
    [](void* data) { delete static_cast<Encoder*>(data); },
};

const webrtc_VideoEncoderFactory_cbs factoryCallbacks{
    [](void*) {
        auto* formats = webrtc_SdpVideoFormat_vector_new();
        auto* retained = webrtc_SdpVideoFormat_new("H264", 4);
        auto* format = webrtc_SdpVideoFormat_unique_get(retained);
        auto* parameters = webrtc_SdpVideoFormat_get_parameters(format);
        std_map_string_string_set(parameters, "packetization-mode", 18, "1", 1);
        std_map_string_string_set(parameters, "profile-level-id", 16, "42e01f", 6);
        std_map_string_string_set(parameters, "level-asymmetry-allowed", 23, "1", 1);
        webrtc_SdpVideoFormat_vector_push_back(formats, format);
        webrtc_SdpVideoFormat_unique_delete(retained);
        return formats;
    },
    [](const webrtc_Environment*, const webrtc_SdpVideoFormat* format, void* data) -> webrtc_VideoEncoder_unique* {
        const auto* name = webrtc_SdpVideoFormat_get_name_const(format);
        if (std::string_view(std_string_c_str(name), std_string_size(name)) != "H264") return nullptr;
        auto state = std::make_unique<Encoder>();
        state->callbacks = *static_cast<Callbacks*>(data);
        auto* result = webrtc_VideoEncoder_new(&encoderCallbacks, state.get());
        if (result) state.release();
        return result;
    },
    [](void* data) { delete static_cast<Callbacks*>(data); },
};

}  // namespace

webrtc_VideoEncoderFactory_unique* createFfmpegEncoderFactory(
    std::function<void(QString)> onError, std::function<void(QString)> onBackend) {
    auto callbacks = std::make_unique<Callbacks>(Callbacks{std::move(onError), std::move(onBackend)});
    auto* factory = webrtc_VideoEncoderFactory_new(&factoryCallbacks, callbacks.get());
    if (factory) callbacks.release();
    return factory;
}

}  // namespace evp
