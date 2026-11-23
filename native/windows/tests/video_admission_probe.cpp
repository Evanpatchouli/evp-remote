// Exercise the actual codec callback without constructing a PeerConnection.
#include "../capture/webrtc_ffmpeg_encoder.cpp"

#include <QJsonObject>
#include <cstdio>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = evp::SendQueuePressure::Clock;

QJsonArray stats(QString id, int packets, double seconds) {
    return {QJsonObject{{"type", "outbound-rtp"}, {"kind", "video"}, {"id", id},
                        {"packetsSent", packets}, {"totalPacketSendDelay", seconds}}};
}

bool checkPressure(evp::SendQueuePressure& pressure, Clock::time_point now) {
    pressure.observe(stats("screen", 100, 0.1), now - 1s);
    pressure.observe(stats("screen", 110, 1.1), now - 500ms);
    if (pressure.active(now)) return false;
    pressure.observe(stats("screen", 120, 2.1), now);
    if (!pressure.active(now) || pressure.active(now + 1100ms)) return false;
    pressure.observe(stats("screen", 125, 2.11), now + 500ms);
    if (pressure.active(now + 500ms)) return false;
    pressure.observe(stats("other", 400, 40), now + 1s);
    pressure.observe(stats("other", 401, 40.2), now + 1500ms);
    if (pressure.active(now + 1500ms)) return false;
    pressure.observe({}, now + 2s);
    return !pressure.active(now + 2s);
}

struct Output {
    struct Frame { uint32_t rtp; bool key; int qp; };
    std::vector<Frame> frames;
    bool skip = false;
};

bool checkCodec() {
    evp::Encoder encoder;
    if (evp::packetQp(encoder.packet.get()) != -1) return false;
    auto* rawQuality = av_packet_new_side_data(encoder.packet.get(), AV_PKT_DATA_QUALITY_STATS, 8);
    if (!rawQuality) return false;
    AV_WL32(rawQuality, 52 * FF_QP2LAMBDA);
    if (evp::packetQp(encoder.packet.get()) != -1) return false;
    av_packet_unref(encoder.packet.get());
    encoder.width = 320;
    encoder.height = 180;
    encoder.bitRate = 1'200'000;
    auto pressure = std::make_shared<evp::SendQueuePressure>();
    encoder.callbacks.sendPressure = pressure;
    if (!encoder.open(evp::x264)) return false;
    const std::unique_ptr<webrtc_VideoEncoder_EncoderInfo_unique, decltype(&webrtc_VideoEncoder_EncoderInfo_unique_delete)>
        info(evp::encoderCallbacks.GetEncoderInfo(&encoder), webrtc_VideoEncoder_EncoderInfo_unique_delete);
    const std::unique_ptr<webrtc_VideoEncoder_QpThresholds, decltype(&webrtc_VideoEncoder_QpThresholds_delete)>
        thresholds(webrtc_VideoEncoder_QpThresholds_new(), webrtc_VideoEncoder_QpThresholds_delete);
    int enabled = 0;
    webrtc_VideoEncoder_ScalingSettings_get_thresholds(webrtc_VideoEncoder_EncoderInfo_get_scaling_settings(
        webrtc_VideoEncoder_EncoderInfo_unique_get(info.get())), &enabled, thresholds.get());
    if (!enabled || webrtc_VideoEncoder_QpThresholds_get_low(thresholds.get()) != 24 ||
        webrtc_VideoEncoder_QpThresholds_get_high(thresholds.get()) != 37) return false;
    Output output;
    const webrtc_VideoEncoder_EncodedImageCallback_cbs hooks{
        [](const webrtc_EncodedImage* image, const webrtc_CodecSpecificInfo*, void* data) {
            auto& out = *static_cast<Output*>(data);
            out.frames.push_back({webrtc_EncodedImage_rtp_timestamp(image),
                                  webrtc_EncodedImage_frame_type(image) == webrtc_VideoFrameType_Key,
                                  webrtc_EncodedImage_qp(image)});
            auto* result = webrtc_VideoEncoder_EncodedImageCallback_Result_new(webrtc_VideoEncoder_EncodedImageCallback_Result_Error_OK);
            webrtc_VideoEncoder_EncodedImageCallback_Result_set_drop_next_frame(
                webrtc_VideoEncoder_EncodedImageCallback_Result_unique_get(result), std::exchange(out.skip, false));
            return result;
        },
        [](void*) {}
    };
    const std::unique_ptr<webrtc_VideoEncoder_EncodedImageCallback, decltype(&webrtc_VideoEncoder_EncodedImageCallback_delete)>
        callback(webrtc_VideoEncoder_EncodedImageCallback_new(&hooks, &output), webrtc_VideoEncoder_EncodedImageCallback_delete);
    encoder.callback = callback.get();
    auto* retained = webrtc_I420Buffer_Create(encoder.width, encoder.height);
    auto* i420 = webrtc_I420Buffer_refcounted_get(retained);
    auto* luma = webrtc_I420Buffer_MutableDataY(i420);
    for (int y = 0; y < encoder.height; ++y) {
        for (int x = 0; x < encoder.width; ++x)
            luma[y * webrtc_I420Buffer_StrideY(i420) + x] = static_cast<uint8_t>((x * 53 + y * 97) % 224 + 16);
    }
    std::fill_n(webrtc_I420Buffer_MutableDataU(i420), webrtc_I420Buffer_StrideU(i420) * encoder.height / 2, 128);
    std::fill_n(webrtc_I420Buffer_MutableDataV(i420), webrtc_I420Buffer_StrideV(i420) * encoder.height / 2, 128);
    auto* source = webrtc_I420Buffer_refcounted_cast_to_webrtc_VideoFrameBuffer(retained);
    const std::unique_ptr<webrtc_VideoFrameBuilder_unique, decltype(&webrtc_VideoFrameBuilder_unique_delete)>
        builder(webrtc_VideoFrameBuilder_new(source), webrtc_VideoFrameBuilder_unique_delete);
    webrtc_I420Buffer_Release(i420);
    webrtc_VideoFrameBuffer_Release(webrtc_VideoFrameBuffer_refcounted_get(source));
    auto push = [&](int ms, bool key = false) {
        auto* spec = webrtc_VideoFrameBuilder_unique_get(builder.get());
        webrtc_VideoFrameBuilder_set_timestamp_us(spec, 1000000 + ms * 1000);
        webrtc_VideoFrameBuilder_set_timestamp_rtp(spec, 90000 + ms * 90);
        const std::unique_ptr<webrtc_VideoFrame_unique, decltype(&webrtc_VideoFrame_unique_delete)>
            frame(webrtc_VideoFrameBuilder_build(spec), webrtc_VideoFrame_unique_delete);
        int emitted = 0;
        return encoder.encode(webrtc_VideoFrame_unique_get(frame.get()), key, emitted);
    };
    output.skip = true;
    if (!push(0) || !push(10) || !push(20) || output.frames.size() != 2) return false;
    const auto now = Clock::now();
    pressure->observe(stats("screen", 0, 0), now - 1s);
    pressure->observe(stats("screen", 10, 1), now - 500ms);
    pressure->observe(stats("screen", 20, 2), now);
    if (!push(30) || !push(40, true) || !push(50) || !push(90) || !push(350) ||
        output.frames.size() != 5 || !output.frames[2].key) return false;
    const int expected[] = {0, 20, 40, 90, 350};
    for (const auto& f : output.frames)
        std::printf("rtp=%u qp=%d key=%d\n", f.rtp, f.qp, f.key);
    if (output.frames.front().qp <= 0) return false;
    for (size_t i = 0; i < output.frames.size(); ++i) {
        const auto& f = output.frames[i];
        if (f.rtp != 90000 + expected[i] * 90 || f.qp < 0 || f.qp > 51) return false;
    }
    // Model a temporarily saturated asynchronous backend with outstanding stamps.
    for (size_t i = 0; i < evp::maxPendingFrames; ++i)
        encoder.pending.push_back({static_cast<int64_t>(i), 0, Clock::now()});
    if (!push(380, true) || !encoder.firstFrame || output.frames.size() != 5) return false;
    encoder.pending.front().submitted -= 1s;
    if (push(450)) return false;
    encoder.pending.clear();
    return push(470) && output.frames.size() == 6 && output.frames.back().key;
}
}

int main() {
    evp::SendQueuePressure pressure;
    if (!checkPressure(pressure, Clock::now()) || !checkCodec()) {
        std::fprintf(stderr, "Video admission, recovery or packet QP check failed.\n");
        return 1;
    }
    std::puts("PASS: packet-delay windows, x264 QP, pressure drops, IDR recovery and RTP timestamps.");
    return 0;
}
