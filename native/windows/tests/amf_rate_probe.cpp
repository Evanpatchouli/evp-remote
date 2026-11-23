// The C ABI cannot construct RateControlParameters. Drive the same internal encoder
// that handles WebRTC's callbacks, including its AMF device, Annex B filter and RTP stamps.
#include "../capture/webrtc_ffmpeg_encoder.cpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int width = 1920, height = 1080;
constexpr int highStart = 4, lowStart = 84, frameCount = 116;
constexpr unsigned rtpStep = 22500;  // Four still-desktop frames per second at a 90 kHz RTP clock.
constexpr unsigned rtpStart = 93'000;

std::vector<uint8_t> makeDesktop() {
    // Dense five-by-seven glyphs in rows, with bright, sharp strokes on a dark background.
    constexpr uint8_t digits[][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };
    std::vector<uint8_t> image(width * height, 42);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int row = y % 16, column = x % 8;
            const int digit = (x / 8 + y / 16) % 10;
            if (row < 7 && column < 5 && (digits[digit][row] & (16 >> column)))
                image[y * width + x] = 205;
        }
    }
    return image;
}

struct Sample {
    unsigned timestamp;
    QByteArray bytes;
    bool key;
};

struct Measurements {
    size_t bytes = 0;
    int keys = 0;
    double endPsnr = 0;
};

bool measure(const std::vector<Sample>& packets, const std::vector<uint8_t>& reference) {
    std::unique_ptr<AVCodecContext, evp::FreeCodec> decoder(avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264)));
    std::unique_ptr<AVFrame, evp::FreeFrame> decoded(av_frame_alloc());
    std::unique_ptr<AVPacket, evp::FreePacket> packet(av_packet_alloc());
    if (!decoder || !decoded || !packet || avcodec_open2(decoder.get(), decoder->codec, nullptr) < 0)
        return false;
    Measurements phases[3];
    const unsigned phaseEnds[] = {highStart, lowStart, frameCount};
    unsigned previous = 0;
    for (size_t index = 0; index < packets.size(); ++index) {
        const auto& sample = packets[index];
        if (sample.timestamp <= previous || sample.timestamp < rtpStart ||
            (sample.timestamp - rtpStart) % rtpStep != 0)
            return false;
        previous = sample.timestamp;
        const auto i = (sample.timestamp - rtpStart) / rtpStep;
        if (i >= frameCount) return false;
        const int phase = i < highStart ? 0 : (i < lowStart ? 1 : 2);
        auto& result = phases[phase];
        result.bytes += sample.bytes.size();
        result.keys += sample.key;
        if (av_new_packet(packet.get(), static_cast<int>(sample.bytes.size())) < 0) return false;
        std::memcpy(packet->data, sample.bytes.constData(), sample.bytes.size());
        const auto sent = avcodec_send_packet(decoder.get(), packet.get());
        av_packet_unref(packet.get());
        if (sent < 0 || avcodec_receive_frame(decoder.get(), decoded.get()) < 0 ||
            decoded->width != width || decoded->height != height || decoded->format != AV_PIX_FMT_YUV420P ||
            bool(decoded->flags & AV_FRAME_FLAG_KEY) != sample.key)
            return false;
        if (index + 1 == packets.size() || packets[index + 1].timestamp >= rtpStart + phaseEnds[phase] * rtpStep) {
            // A queued frame may be retired when an asynchronous hardware encoder is replaced.
            if (phaseEnds[phase] - 1 - i > evp::maxPendingFrames) return false;
            double error = 0;
            for (int y = 0; y < height; ++y) {
                auto* row = decoded->data[0] + y * decoded->linesize[0];
                for (int x = 0; x < width; ++x) {
                    const int difference = row[x] - reference[y * width + x];
                    error += difference * difference;
                }
            }
            result.endPsnr = 10 * std::log10(255.0 * 255 * width * height / std::max(1.0, error));
        }
        av_frame_unref(decoded.get());
    }
    for (const auto& phase : phases)
        std::printf("bytes=%zu, keyframes=%d, final luma PSNR=%.2f dB\n", phase.bytes, phase.keys, phase.endPsnr);

    // All phases are shorter than the natural 120-frame GOP. Each rate transition should
    // produce one independently decodable frame, while the small bitrate jitter should not.
    return phases[0].endPsnr > 0 && phases[2].endPsnr > 0 &&
           phases[0].keys == 1 && phases[1].keys == 1 && phases[2].keys == 1 &&
           phases[1].endPsnr > phases[0].endPsnr + 6 && phases[1].endPsnr > phases[2].endPsnr + 6 &&
           phases[1].bytes / (lowStart - highStart) > 2 * (phases[2].bytes / (frameCount - lowStart));
}

}  // namespace

int main() {
    evp::Encoder encoder;
    encoder.width = width;
    encoder.height = height;
    encoder.bitRate = 300'000;
    encoder.frameRate = 60;
    if (!encoder.open(evp::amf)) {
        std::fprintf(stderr, "SKIP: a working FFmpeg AMF encoder on an AMD D3D11 adapter is required.\n");
        return 77;
    }

    std::vector<Sample> packets;
    const webrtc_VideoEncoder_EncodedImageCallback_cbs hooks{
        [](const webrtc_EncodedImage* image, const webrtc_CodecSpecificInfo*, void* state) {
            const auto ref = webrtc_EncodedImage_encoded_data(image);
            auto* buffer = webrtc_EncodedImageBuffer_refcounted_get(ref);
            const auto* bytes = reinterpret_cast<const char*>(webrtc_EncodedImageBuffer_data(buffer));
            static_cast<std::vector<Sample>*>(state)->push_back({
                webrtc_EncodedImage_rtp_timestamp(image),
                QByteArray(bytes, static_cast<qsizetype>(webrtc_EncodedImageBuffer_size(buffer))),
                webrtc_EncodedImage_frame_type(image) == webrtc_VideoFrameType_Key
            });
            webrtc_EncodedImageBuffer_Release(buffer);
            return webrtc_VideoEncoder_EncodedImageCallback_Result_new(webrtc_VideoEncoder_EncodedImageCallback_Result_Error_OK);
        },
        [](void*) {}
    };
    std::unique_ptr<webrtc_VideoEncoder_EncodedImageCallback, decltype(&webrtc_VideoEncoder_EncodedImageCallback_delete)>
        callback(webrtc_VideoEncoder_EncodedImageCallback_new(&hooks, &packets), webrtc_VideoEncoder_EncodedImageCallback_delete);
    encoder.callback = callback.get();
    const auto reference = makeDesktop();
    auto* ref = webrtc_I420Buffer_CreateWithStrides(width, height, width, width / 2, width / 2);
    auto* planes = webrtc_I420Buffer_refcounted_get(ref);
    std::memcpy(webrtc_I420Buffer_MutableDataY(planes), reference.data(), reference.size());
    std::memset(webrtc_I420Buffer_MutableDataU(planes), 128, width * height / 4);
    std::memset(webrtc_I420Buffer_MutableDataV(planes), 128, width * height / 4);
    auto* source = webrtc_I420Buffer_refcounted_cast_to_webrtc_VideoFrameBuffer(ref);
    auto* owner = webrtc_VideoFrameBuilder_new(source);
    auto* builder = webrtc_VideoFrameBuilder_unique_get(owner);
    webrtc_I420Buffer_Release(planes);
    webrtc_VideoFrameBuffer_Release(webrtc_VideoFrameBuffer_refcounted_get(source));
    const auto submit = [&](int index) {
        webrtc_VideoFrameBuilder_set_timestamp_us(builder, 1'000'000 + 250'000LL * index);
        webrtc_VideoFrameBuilder_set_timestamp_rtp(builder, rtpStart + rtpStep * index);
        auto* frame = webrtc_VideoFrameBuilder_build(builder);
        const int status = evp::encoderCallbacks.Encode(webrtc_VideoFrame_unique_get(frame), nullptr, &encoder);
        webrtc_VideoFrame_unique_delete(frame);
        return encoder.active == &evp::amf ? status : webrtc_VideoCodecStatus_EncoderFailure;
    };
    for (int i = 0; i < frameCount; ++i) {
        if (i == highStart || i == lowStart)
            encoder.setRates(i == highStart ? 8'000'000 : 300'000, 60);
        else if (i > highStart && i < lowStart && i % 4 == 0)
            encoder.setRates(i % 8 == 0 ? 7'900'000 : 8'050'000, 60);
        if (submit(i) < 0) return 1;
    }
    int emitted = 0;
    if (!encoder.drain(emitted) || !measure(packets, reference)) return 1;

    // These two requests arrive during the upward-change cooldown. The newest
    // must apply once the window opens even without another SetRates callback.
    encoder.setRates(7'000'000, 60);
    if (submit(frameCount) < 0 || encoder.codec->bit_rate != 7'000'000) return 1;
    encoder.setRates(12'000'000, 60);
    if (submit(frameCount + 1) < 0 || encoder.codec->bit_rate != 7'000'000) return 1;
    encoder.setRates(10'000'000, 60);
    for (int i = 2; i <= 12; ++i)
        if (submit(frameCount + i) < 0 || encoder.codec->bit_rate == 12'000'000) return 1;
    if (encoder.codec->bit_rate != 10'000'000) return 1;

    encoder.setRates(0, 60);
    const size_t beforePause = packets.size();
    if (submit(frameCount + 13) != webrtc_VideoCodecStatus_NoOutput || packets.size() != beforePause) return 1;
    encoder.setRates(10'000'000, 60);
    for (int i = 14; i <= 16; ++i)
        if (submit(frameCount + i) < 0) return 1;
    webrtc_VideoFrameBuilder_unique_delete(owner);
    const unsigned resumeStamp = rtpStart + (frameCount + 14) * rtpStep;
    const auto resumed = std::find_if(packets.begin() + beforePause, packets.end(),
                                      [resumeStamp](const Sample& sample) { return sample.timestamp >= resumeStamp; });
    return resumed != packets.end() && resumed->key && resumed->timestamp == resumeStamp ? 0 : 1;
}
