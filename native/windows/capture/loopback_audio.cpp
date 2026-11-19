#include "loopback_audio.h"

#include <audioclient.h>
#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <vector>

namespace evp {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int sampleRate = 48'000;
constexpr int channels = 2;
constexpr int blockFrames = sampleRate / 100;
constexpr int blockSamples = blockFrames * channels;

void require(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(QStringLiteral("%1 (0x%2)")
            .arg(QString::fromLatin1(operation)).arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'))
            .toStdString());
    }
}

struct ComApartment {
    ComApartment() { require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~ComApartment() { CoUninitialize(); }
};

struct AudioEvent {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~AudioEvent() { if (handle) CloseHandle(handle); }
};

struct AudioThreadPriority {
    DWORD task = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    ~AudioThreadPriority() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};

class AudioConverter {
public:
    explicit AudioConverter(const WAVEFORMATEX& wave) : inputChannels_(wave.nChannels), inputRate_(wave.nSamplesPerSec) {
        GUID subtype = {};
        DWORD channelMask = 0;
        if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            wave.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
            subtype = extended.SubFormat;
            channelMask = extended.dwChannelMask;
        } else if (wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        } else if (wave.wFormatTag == WAVE_FORMAT_PCM) {
            subtype = KSDATAFORMAT_SUBTYPE_PCM;
        }
        if (subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && wave.wBitsPerSample == 32) {
            format_ = AV_SAMPLE_FMT_FLT;
        } else if (subtype == KSDATAFORMAT_SUBTYPE_PCM) {
            switch (wave.wBitsPerSample) {
            case 8: format_ = AV_SAMPLE_FMT_U8; break;
            case 16: format_ = AV_SAMPLE_FMT_S16; break;
            case 24: packed24_ = true; [[fallthrough]];
            case 32: format_ = AV_SAMPLE_FMT_S32; break;
            }
        }
        if (format_ == AV_SAMPLE_FMT_NONE || inputChannels_ < 1 || inputChannels_ > 32 ||
            inputRate_ < 8'000 || inputRate_ > 384'000 || wave.nBlockAlign != inputChannels_ * wave.wBitsPerSample / 8) {
            throw std::runtime_error("Unsupported WASAPI render mix format");
        }

        AVChannelLayout input{};
        if (channelMask && av_channel_layout_from_mask(&input, channelMask) == 0 && input.nb_channels != inputChannels_) {
            av_channel_layout_uninit(&input);
        }
        if (!input.nb_channels) av_channel_layout_default(&input, inputChannels_);
        const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* context = nullptr;
        const auto result = swr_alloc_set_opts2(&context, &stereo, AV_SAMPLE_FMT_S16, sampleRate,
                                                &input, format_, inputRate_, 0, nullptr);
        av_channel_layout_uninit(&input);
        context_.reset(context);
        if (result < 0 || swr_init(context_.get()) < 0) {
            throw std::runtime_error("FFmpeg loopback audio conversion unavailable");
        }
    }

    void append(const BYTE* data, int frames, DWORD flags) {
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            pending_ = 0;
        }
        const auto samples = static_cast<std::size_t>(frames) * inputChannels_;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            silence_.assign(samples * av_get_bytes_per_sample(format_), format_ == AV_SAMPLE_FMT_U8 ? 128 : 0);
            data = silence_.data();
        } else if (packed24_) {
            unpacked_.resize(samples);
            for (std::size_t i = 0; i < samples; ++i, data += 3) {
                unpacked_[i] = static_cast<std::int32_t>(std::uint32_t(data[0]) << 8 |
                    std::uint32_t(data[1]) << 16 | std::uint32_t(data[2]) << 24);
            }
            data = reinterpret_cast<const BYTE*>(unpacked_.data());
        }
        const auto available = swr_get_out_samples(context_.get(), frames);
        if (available <= 0 || available > sampleRate * 2) throw std::runtime_error("Invalid WASAPI packet duration");
        converted_.resize(static_cast<std::size_t>(available) * channels);
        auto* output = reinterpret_cast<std::uint8_t*>(converted_.data());
        const auto produced = swr_convert(context_.get(), &output, available, &data, frames);
        if (produced < 0) throw std::runtime_error("FFmpeg loopback audio conversion failed");
        const auto producedSamples = produced * channels;
        const auto capacity = static_cast<int>(queue_.size());
        const auto keepIncoming = std::min(capacity, producedSamples);
        const auto keepPending = std::min(capacity - keepIncoming, pending_);
        if (keepPending < pending_) {
            std::move(queue_.begin() + pending_ - keepPending, queue_.begin() + pending_, queue_.begin());
        }
        std::copy_n(converted_.data() + producedSamples - keepIncoming, keepIncoming, queue_.data() + keepPending);
        pending_ = keepPending + keepIncoming;
    }

    void deliver(const LoopbackAudio::AudioCallback& callback) {
        std::array<std::int16_t, blockSamples> output{};
        const auto count = std::min(pending_, blockSamples);
        std::copy_n(queue_.data(), count, output.data());
        callback(output.data(), blockFrames);
        if (count) std::move(queue_.begin() + count, queue_.begin() + pending_, queue_.begin());
        pending_ -= count;
    }

private:
    struct FreeConverter {
        void operator()(SwrContext* context) const { swr_free(&context); }
    };
    std::unique_ptr<SwrContext, FreeConverter> context_;
    std::array<std::int16_t, blockSamples * 4> queue_{};
    std::vector<std::int16_t> converted_;
    std::vector<std::int32_t> unpacked_;
    std::vector<BYTE> silence_;
    int pending_ = 0;
    int inputChannels_;
    int inputRate_;
    AVSampleFormat format_ = AV_SAMPLE_FMT_NONE;
    bool packed24_ = false;
};

void run(std::stop_token stop, const LoopbackAudio::AudioCallback& onAudio, bool& started) {
    const ComApartment apartment;
    const AudioEvent event;
    if (!event.handle) require(HRESULT_FROM_WIN32(GetLastError()), "WASAPI CreateEvent");
    std::stop_callback wake(stop, [&event] { SetEvent(event.handle); });
    ComPtr<IMMDeviceEnumerator> devices;
    require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices)),
            "WASAPI MMDeviceEnumerator");
    ComPtr<IMMDevice> endpoint;
    require(devices->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint), "WASAPI default render endpoint");
    ComPtr<IAudioClient> client;
    require(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &client), "WASAPI IAudioClient");
    WAVEFORMATEX* rawMixFormat = nullptr;
    require(client->GetMixFormat(&rawMixFormat), "WASAPI GetMixFormat");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawMixFormat, CoTaskMemFree);
    AudioConverter converter(*format);
    require(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               0, 0, format.get(), nullptr), "WASAPI Initialize loopback");
    require(client->SetEventHandle(event.handle), "WASAPI SetEventHandle");
    UINT32 bufferFrames = 0;
    require(client->GetBufferSize(&bufferFrames), "WASAPI GetBufferSize");
    ComPtr<IAudioCaptureClient> capture;
    require(client->GetService(IID_PPV_ARGS(&capture)), "WASAPI IAudioCaptureClient");
    if (stop.stop_requested()) return;
    const AudioThreadPriority priority;
    require(client->Start(), "WASAPI Start loopback");
    started = true;
    struct StopClient {
        IAudioClient* client;
        ~StopClient() { client->Stop(); }
    } stopClient{client.Get()};

    auto deadline = Clock::now() + 10ms;
    while (!stop.stop_requested()) {
        const auto timeout = std::max(0ms, std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()));
        if (WaitForSingleObject(event.handle, static_cast<DWORD>(timeout.count())) == WAIT_FAILED) {
            require(HRESULT_FROM_WIN32(GetLastError()), "WASAPI wait");
        }
        while (!stop.stop_requested()) {
            UINT32 packet = 0;
            require(capture->GetNextPacketSize(&packet), "WASAPI GetNextPacketSize");
            if (!packet) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            require(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "WASAPI GetBuffer");
            struct ReleasePacket {
                IAudioCaptureClient* capture;
                UINT32 frames;
                ~ReleasePacket() { if (capture) capture->ReleaseBuffer(frames); }
            } release{capture.Get(), frames};
            if (frames > bufferFrames) throw std::runtime_error("Invalid WASAPI packet size");
            if (frames) converter.append(data, static_cast<int>(frames), flags);
            release.capture = nullptr;
            require(capture->ReleaseBuffer(frames), "WASAPI ReleaseBuffer");
        }

        const auto now = Clock::now();
        if (now >= deadline && !stop.stop_requested()) {
            converter.deliver(onAudio);
            deadline = std::max(deadline + 10ms, now + 1ms);
        }
    }
}

}  // namespace

LoopbackAudio::~LoopbackAudio() { stop(); }

bool LoopbackAudio::start(AudioCallback onAudio, ErrorCallback onError, ErrorCallback onRuntimeError) {
    if (worker_.joinable() || !onAudio) return false;
    try {
        worker_ = std::jthread([onAudio = std::move(onAudio), onError = std::move(onError),
                               onRuntimeError = std::move(onRuntimeError)](std::stop_token stop) {
            bool started = false;
            try {
                run(stop, onAudio, started);
            } catch (const std::exception& error) {
                const auto& notify = started && onRuntimeError ? onRuntimeError : onError;
                if (!stop.stop_requested() && notify) notify(QString::fromUtf8(error.what()));
            }
        });
    } catch (const std::system_error&) {
        return false;
    }
    return true;
}

void LoopbackAudio::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

}  // namespace evp
