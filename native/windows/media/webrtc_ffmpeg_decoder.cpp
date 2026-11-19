#include "webrtc_ffmpeg_decoder.h"

#include "ffmpeg_decoder.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <webrtc_c/api/video/i420_buffer.h>
#include <webrtc_c/api/video_codecs/video_codec.h>
#include <webrtc_c/api/video_codecs/video_decoder_factory.h>
#include <webrtc_c/api/video_codecs/video_encoder.h>

#include <memory>
#include <string_view>

namespace evp {
namespace {

using Microsoft::WRL::ComPtr;

webrtc_I420Buffer_refcounted* toI420(void* userData) {
    const auto& frame = *static_cast<const DecodedVideoFrame*>(userData);
    auto* output = webrtc_I420Buffer_Create(frame.width(), frame.height());
    if (!output) return nullptr;
    auto* buffer = webrtc_I420Buffer_refcounted_get(output);
    if (frame.copyI420(webrtc_I420Buffer_MutableDataY(buffer), webrtc_I420Buffer_StrideY(buffer),
                       webrtc_I420Buffer_MutableDataU(buffer), webrtc_I420Buffer_StrideU(buffer),
                       webrtc_I420Buffer_MutableDataV(buffer), webrtc_I420Buffer_StrideV(buffer))) {
        return output;
    }
    webrtc_I420Buffer_Release(buffer);
    return nullptr;
}

const webrtc_VideoFrameBuffer_cbs frameCallbacks{
    [](void*) { return webrtc_VideoFrameBuffer_Type_kNative; },
    [](void* data) { return static_cast<const DecodedVideoFrame*>(data)->width(); },
    [](void* data) { return static_cast<const DecodedVideoFrame*>(data)->height(); },
    toI420,
    [](webrtc_VideoFrameBuffer* buffer, int x, int y, int width, int height, int outWidth, int outHeight, void*) {
        return webrtc_VideoFrameBuffer_DefaultCropAndScale(buffer, x, y, width, height, outWidth, outHeight);
    },
    [](void* data) { delete static_cast<DecodedVideoFrame*>(data); },
};

struct Decoder {
    explicit Decoder(ID3D11Device* device) : device(device) {}

    bool publish(const DecodedVideoFrame& decoded) {
        auto native = std::make_unique<DecodedVideoFrame>(decoded);
        auto* retained = webrtc_VideoFrameBuffer_make_ref_counted(&frameCallbacks, native.get());
        if (!retained) return false;
        native.release();
        const std::unique_ptr<webrtc_VideoFrameBuilder_unique, decltype(&webrtc_VideoFrameBuilder_unique_delete)> builder(
            webrtc_VideoFrameBuilder_new(retained), webrtc_VideoFrameBuilder_unique_delete);
        webrtc_VideoFrameBuffer_Release(webrtc_VideoFrameBuffer_refcounted_get(retained));
        if (!builder) return false;
        auto* spec = webrtc_VideoFrameBuilder_unique_get(builder.get());
        webrtc_VideoFrameBuilder_set_timestamp_rtp(spec, decoded.rtpTimestamp());
        const std::unique_ptr<webrtc_VideoFrame_unique, decltype(&webrtc_VideoFrame_unique_delete)> output(
            webrtc_VideoFrameBuilder_build(spec), webrtc_VideoFrame_unique_delete);
        if (!output) return false;
        sawOutput = true;
        hardware = decoded.isHardware();
        webrtc_VideoDecoder_DecodedImageCallback_Decoded(callback, webrtc_VideoFrame_unique_get(output.get()));
        return true;
    }

    bool configure(bool preferHardware) {
        codec = std::make_unique<FfmpegDecoder>(device.Get(), [this](const DecodedVideoFrame& frame) {
            return publish(frame);
        });
        sawOutput = false;
        hardware = false;
        return codec->openH264(preferHardware);
    }

    ComPtr<ID3D11Device> device;
    std::unique_ptr<FfmpegDecoder> codec;
    webrtc_VideoDecoder_DecodedImageCallback* callback = nullptr;
    bool sawOutput = false;
    bool hardware = false;
};

const webrtc_VideoDecoder_cbs decoderCallbacks{
    [](const webrtc_VideoDecoder_Settings* settings, void* data) -> int {
        if (webrtc_VideoDecoder_Settings_codec_type(settings) != webrtc_VideoCodecType_H264) return false;
        return static_cast<Decoder*>(data)->configure(true);
    },
    [](const webrtc_EncodedImage* image, std::int64_t, void* data) -> std::int32_t {
        auto& decoder = *static_cast<Decoder*>(data);
        if (!decoder.codec || !decoder.callback) return webrtc_VideoCodecStatus_Uninitialized;
        if (decoder.codec->hasHardwareDevice() && FAILED(decoder.device->GetDeviceRemovedReason())) {
            if (webrtc_EncodedImage_frame_type(image) != webrtc_VideoFrameType_Key) return webrtc_VideoCodecStatus_Error;
            if (!decoder.configure(false)) return webrtc_VideoCodecStatus_Error;
        }
        auto* retained = webrtc_EncodedImage_encoded_data(image);
        if (!retained) return webrtc_VideoCodecStatus_ErrParameter;
        const std::unique_ptr<webrtc_EncodedImageBuffer, decltype(&webrtc_EncodedImageBuffer_Release)> buffer(
            webrtc_EncodedImageBuffer_refcounted_get(retained), webrtc_EncodedImageBuffer_Release);
        const auto bytes = std::span(webrtc_EncodedImageBuffer_data(buffer.get()),
                                     webrtc_EncodedImageBuffer_size(buffer.get()));
        const auto submitted = decoder.codec->stats().submitted;
        if (!decoder.codec->decodeAnnexB(bytes, webrtc_EncodedImage_rtp_timestamp(image))) {
            return webrtc_VideoCodecStatus_Error;
        }
        return decoder.codec->stats().submitted != submitted ? webrtc_VideoCodecStatus_Ok : webrtc_VideoCodecStatus_NoOutput;
    },
    [](webrtc_VideoDecoder_DecodedImageCallback* callback, void* data) -> std::int32_t {
        static_cast<Decoder*>(data)->callback = callback;
        return webrtc_VideoCodecStatus_Ok;
    },
    [](void* data) -> std::int32_t {
        static_cast<Decoder*>(data)->codec.reset();
        return webrtc_VideoCodecStatus_Ok;
    },
    [](void* data) {
        const auto& decoder = *static_cast<const Decoder*>(data);
        auto* result = webrtc_VideoDecoder_DecoderInfo_new();
        auto* info = webrtc_VideoDecoder_DecoderInfo_unique_get(result);
        const bool hardware = decoder.sawOutput ? decoder.hardware
            : decoder.codec && decoder.codec->hasHardwareDevice();
        const std::unique_ptr<std_string_unique, decltype(&std_string_unique_delete)> name(
            std_string_new_from_cstr(hardware ? "FFmpeg D3D11VA" : "FFmpeg H.264"), std_string_unique_delete);
        webrtc_VideoDecoder_DecoderInfo_set_implementation_name(info, std_string_unique_get(name.get()));
        webrtc_VideoDecoder_DecoderInfo_set_is_hardware_accelerated(info, hardware);
        return result;
    },
    [](void* data) { delete static_cast<Decoder*>(data); },
};

bool isH264(const webrtc_SdpVideoFormat* format) {
    const auto* name = webrtc_SdpVideoFormat_get_name_const(format);
    return std::string_view(std_string_c_str(name), std_string_size(name)) == "H264";
}

struct Factory {
    explicit Factory(ID3D11Device* device) : device(device), builtin(webrtc_CreateBuiltinVideoDecoderFactory()) {}
    ~Factory() { webrtc_VideoDecoderFactory_unique_delete(builtin); }

    webrtc_VideoDecoderFactory* fallback() const { return webrtc_VideoDecoderFactory_unique_get(builtin); }

    ComPtr<ID3D11Device> device;
    webrtc_VideoDecoderFactory_unique* builtin;
};

void addH264Format(webrtc_SdpVideoFormat_vector* formats, const char* profile) {
    auto* unique = webrtc_SdpVideoFormat_new("H264", 4);
    auto* format = webrtc_SdpVideoFormat_unique_get(unique);
    auto* parameters = webrtc_SdpVideoFormat_get_parameters(format);
    std_map_string_string_set(parameters, "packetization-mode", 18, "1", 1);
    std_map_string_string_set(parameters, "profile-level-id", 16, profile, 6);
    std_map_string_string_set(parameters, "level-asymmetry-allowed", 23, "1", 1);
    webrtc_SdpVideoFormat_vector_push_back(formats, format);
    webrtc_SdpVideoFormat_unique_delete(unique);
}

const webrtc_VideoDecoderFactory_cbs factoryCallbacks{
    [](void* data) {
        auto* formats = webrtc_VideoDecoderFactory_GetSupportedFormats(static_cast<Factory*>(data)->fallback());
        for (int i = 0; i < webrtc_SdpVideoFormat_vector_size(formats); ++i) {
            if (isH264(webrtc_SdpVideoFormat_vector_get_const(formats, i))) return formats;
        }
        addH264Format(formats, "42e01f");
        addH264Format(formats, "640c1f");
        return formats;
    },
    [](const webrtc_Environment* env, const webrtc_SdpVideoFormat* format, void* data) {
        auto& factory = *static_cast<Factory*>(data);
        if (!isH264(format)) return webrtc_VideoDecoderFactory_Create(factory.fallback(), env, format);
        auto state = std::make_unique<Decoder>(factory.device.Get());
        auto* decoder = webrtc_VideoDecoder_new(&decoderCallbacks, state.get());
        if (decoder) state.release();
        return decoder;
    },
    [](void* data) { delete static_cast<Factory*>(data); },
};

}  // namespace

webrtc_VideoDecoderFactory_unique* makeFfmpegDecoderFactory(ID3D11Device* device) {
    auto state = std::make_unique<Factory>(device);
    if (!state->builtin) return nullptr;
    auto* factory = webrtc_VideoDecoderFactory_new(&factoryCallbacks, state.get());
    if (factory) state.release();
    return factory;
}

const DecodedVideoFrame* getFfmpegDecodedFrame(const webrtc_VideoFrameBuffer* buffer) {
    if (!buffer || webrtc_VideoFrameBuffer_type(buffer) != webrtc_VideoFrameBuffer_Type_kNative) return nullptr;
    return static_cast<const DecodedVideoFrame*>(webrtc_VideoFrameBuffer_get_user_data(buffer));
}

}  // namespace evp
