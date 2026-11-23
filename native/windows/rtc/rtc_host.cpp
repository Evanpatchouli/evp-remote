#include "rtc_host.h"
#include "cursor_outbox.h"
#include "rtc_runtime.h"

#include "../capture/desktop_capture.h"
#include "../capture/loopback_audio.h"
#include "../capture/send_queue_pressure.h"
#include "../capture/webrtc_ffmpeg_encoder.h"
#include "../media/video_timing.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>

namespace evp {
namespace {

template<auto Delete>
struct Deleter {
    template<class T>
    void operator()(T* value) const { Delete(value); }
};

template<class T, auto Delete>
using Owned = std::unique_ptr<T, Deleter<Delete>>;

template<auto Delete, class T>
auto own(T* value) { return Owned<T, Delete>(value); }

std::string takeString(std_string_unique* value) {
    const auto owner = own<std_string_unique_delete>(value);
    return value ? std_string_c_str(std_string_unique_get(value)) : "";
}

std::string errorDetail(webrtc_RTCError_unique* error) {
    const auto owned = own<webrtc_RTCError_unique_delete>(error);
    if (!error || webrtc_RTCError_ok(webrtc_RTCError_unique_get(error))) return {};
    const char* message = nullptr;
    std::size_t size = 0;
    webrtc_RTCError_message(webrtc_RTCError_unique_get(error), &message, &size);
    return message && size ? " " + std::string(message, size) : " Unknown WebRTC error.";
}

const char* connectionStateName(int state) {
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kNew) return "new";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kConnecting) return "connecting";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kConnected) return "connected";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kDisconnected) return "disconnected";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kClosed) return "closed";
    return "failed";
}

bool isCandidateType(const QString& type) {
    return type == "host" || type == "srflx" || type == "prflx" || type == "relay";
}

RouteStats findRoute(const QJsonArray& reports) {
    QHash<QString, QJsonObject> byId;
    for (const auto& entry : reports) {
        const auto report = entry.toObject();
        byId.insert(report.value("id").toString(), report);
    }
    for (const auto& entry : reports) {
        const auto transport = entry.toObject();
        if (transport.value("type") != "transport") continue;
        const auto pair = byId.value(transport.value("selectedCandidatePairId").toString());
        const auto local = byId.value(pair.value("localCandidateId").toString()).value("candidateType").toString();
        const auto remote = byId.value(pair.value("remoteCandidateId").toString()).value("candidateType").toString();
        if (isCandidateType(local) && isCandidateType(remote)) {
            return {local == "relay" || remote == "relay" ? "relay" : "direct",
                    local.toStdString(), remote.toStdString()};
        }
    }
    return {};
}

constexpr std::size_t maxPendingCandidates = 256;
constexpr std::size_t maxInputMessage = 256;
constexpr unsigned maxQueuedInputMessages = 256;
constexpr std::string_view inputLabel = "input";
constexpr std::string_view streamId = "desktop";

class WindowsHostMedia final : public HostMediaSource {
public:
    bool startVideo(webrtc_AdaptedVideoTrackSource* source, int maxFps, Error error, VideoSize size) override {
        return desktop_.start(source, maxFps, asQtCallback(std::move(error)), std::move(size), std::move(cursor_));
    }

    void setVideoMaxFps(int maxFps) override { desktop_.setMaxFps(maxFps); }
    bool supportsSeparateCursor() const override { return true; }
    void setCursorCallback(Cursor cursor) override { cursor_ = std::move(cursor); }
    void setCursorEpoch(quint64 epoch) override { desktop_.setCursorEpoch(epoch); }
    void noteCursorMove(quint64 epoch, quint32 serial, QPointF point) override {
        desktop_.noteCursorMove(epoch, serial, point);
    }

    bool startAudio(Pcm pcm, Error startupResult, Error runtimeError) override {
        return audio_.start(std::move(pcm), asQtCallback(std::move(startupResult)), asQtCallback(std::move(runtimeError)));
    }

    void stop() override {
        desktop_.stop();
        audio_.stop();
    }

private:
    static auto asQtCallback(Error callback) -> std::function<void(QString)> {
        return [callback = std::move(callback)](const QString& message) { callback(message.toStdString()); };
    }

    DesktopCapture desktop_;
    LoopbackAudio audio_;
    Cursor cursor_;
};

bool isOpus(const webrtc_SdpAudioFormat* format) {
    return _stricmp(std_string_c_str(webrtc_SdpAudioFormat_get_name_const(format)), "opus") == 0;
}

webrtc_AudioEncoderFactory_refcounted* createOpusFactory() {
    // Keep the upstream Opus implementation and its SDP parameters, while
    // restricting this send-only peer to the codec used for system audio.
    static const webrtc_AudioEncoderFactory_cbs callbacks{
        .GetSupportedEncoders = [](void* data) {
            const auto supported = own<webrtc_AudioCodecSpec_vector_delete>(
                webrtc_AudioEncoderFactory_GetSupportedEncoders(static_cast<webrtc_AudioEncoderFactory*>(data)));
            auto* formats = webrtc_AudioCodecSpec_vector_new();
            for (int index = 0; index < webrtc_AudioCodecSpec_vector_size(supported.get()); ++index) {
                auto* codec = webrtc_AudioCodecSpec_vector_get(supported.get(), index);
                if (isOpus(webrtc_AudioCodecSpec_get_format(codec))) webrtc_AudioCodecSpec_vector_push_back(formats, codec);
            }
            return formats;
        },
        .QueryAudioEncoder = [](const webrtc_SdpAudioFormat* format, void* data) {
            return isOpus(format) ? webrtc_AudioEncoderFactory_QueryAudioEncoder(
                static_cast<webrtc_AudioEncoderFactory*>(data), format) : nullptr;
        },
        .Create = [](const webrtc_Environment* env, const webrtc_SdpAudioFormat* format,
                     webrtc_AudioEncoderFactory_Options* options, void* data) {
            return isOpus(format) ? webrtc_AudioEncoderFactory_Create(
                static_cast<webrtc_AudioEncoderFactory*>(data), env, format, options) : nullptr;
        },
        .OnDestroy = [](void* data) { webrtc_AudioEncoderFactory_Release(static_cast<webrtc_AudioEncoderFactory*>(data)); },
    };
    auto* builtin = webrtc_AudioEncoderFactory_refcounted_get(webrtc_CreateBuiltinAudioEncoderFactory());
    if (!builtin) return nullptr;
    auto* factory = webrtc_AudioEncoderFactory_make_ref_counted(&callbacks, builtin);
    if (!factory) webrtc_AudioEncoderFactory_Release(builtin);
    return factory;
}

// This device module receives PCM solely from the session's loopback producer.
// It never opens a Windows recording or playout endpoint itself.
class LoopbackDevice final {
public:
    webrtc_AudioDeviceModule_refcounted* create() {
        static webrtc_AudioDeviceModule_cbs callbacks{
            .ActiveAudioLayer = [](int* layer, void*) -> std::int32_t {
                if (layer) *layer = webrtc_AudioDeviceModule_kDummyAudio;
                return 0;
            },
            .RegisterAudioCallback = [](webrtc_AudioTransport* transport, void* data) -> std::int32_t {
                auto& self = *static_cast<LoopbackDevice*>(data);
                const std::scoped_lock lock(self.mutex_);
                self.transport_ = transport;
                return 0;
            },
            .Init = [](void* data) -> std::int32_t { return static_cast<LoopbackDevice*>(data)->init(); },
            .Terminate = [](void* data) -> std::int32_t { return static_cast<LoopbackDevice*>(data)->terminate(); },
            .Initialized = [](void* data) -> int { return static_cast<LoopbackDevice*>(data)->initialized_; },
            .PlayoutDevices = [](void*) -> std::int16_t { return 0; },
            .RecordingDevices = [](void*) -> std::int16_t { return 1; },
            .PlayoutDeviceName = deviceNameUnavailable,
            .RecordingDeviceName = [](std::uint16_t index, char* name, char* guid, void*) -> std::int32_t {
                if (index != 0) return -1;
                if (name) std::memcpy(name, "System loopback", sizeof("System loopback"));
                if (guid) *guid = '\0';
                return 0;
            },
            .SetPlayoutDevice = [](std::uint16_t, void*) -> std::int32_t { return -1; },
            .SetPlayoutDeviceWithWindowsDeviceType = [](int, void*) -> std::int32_t { return -1; },
            .SetRecordingDevice = [](std::uint16_t index, void*) -> std::int32_t { return index == 0 ? 0 : -1; },
            .SetRecordingDeviceWithWindowsDeviceType = [](int, void*) -> std::int32_t { return 0; },
            .PlayoutIsAvailable = unavailable,
            .InitPlayout = unsupported,
            .PlayoutIsInitialized = no,
            .RecordingIsAvailable = available,
            .InitRecording = [](void* data) -> std::int32_t {
                static_cast<LoopbackDevice*>(data)->recordingInitialized_ = true;
                return 0;
            },
            .RecordingIsInitialized = [](void* data) -> int { return static_cast<LoopbackDevice*>(data)->recordingInitialized_; },
            .StartPlayout = unsupported,
            .StopPlayout = noop,
            .Playing = no,
            .StartRecording = [](void* data) -> std::int32_t { return static_cast<LoopbackDevice*>(data)->record(true); },
            .StopRecording = [](void* data) -> std::int32_t { return static_cast<LoopbackDevice*>(data)->record(false); },
            .Recording = [](void* data) -> int { return static_cast<LoopbackDevice*>(data)->recording(); },
            .InitSpeaker = unsupported,
            .SpeakerIsInitialized = no,
            .InitMicrophone = noop,
            .MicrophoneIsInitialized = [](void* data) -> int { return static_cast<LoopbackDevice*>(data)->initialized_; },
            .SpeakerVolumeIsAvailable = unavailable,
            .SetSpeakerVolume = unsupportedVolume,
            .SpeakerVolume = volumeUnavailable,
            .MaxSpeakerVolume = volumeUnavailable,
            .MinSpeakerVolume = volumeUnavailable,
            .MicrophoneVolumeIsAvailable = unavailable,
            .SetMicrophoneVolume = unsupportedVolume,
            .MicrophoneVolume = volumeUnavailable,
            .MaxMicrophoneVolume = volumeUnavailable,
            .MinMicrophoneVolume = volumeUnavailable,
            .SpeakerMuteIsAvailable = unavailable,
            .SetSpeakerMute = unsupportedToggle,
            .SpeakerMute = unavailable,
            .MicrophoneMuteIsAvailable = unavailable,
            .SetMicrophoneMute = unsupportedToggle,
            .MicrophoneMute = unavailable,
            .StereoPlayoutIsAvailable = unavailable,
            .SetStereoPlayout = unsupportedToggle,
            .StereoPlayout = unavailable,
            .StereoRecordingIsAvailable = available,
            .SetStereoRecording = [](int, void*) -> std::int32_t { return 0; },
            .StereoRecording = available,
            .PlayoutDelay = [](std::uint16_t* delay, void*) -> std::int32_t {
                if (delay) *delay = 0;
                return 0;
            },
            .BuiltInAECIsAvailable = no,
            .BuiltInAGCIsAvailable = no,
            .BuiltInNSIsAvailable = no,
            .EnableBuiltInAEC = unsupportedToggle,
            .EnableBuiltInAGC = unsupportedToggle,
            .EnableBuiltInNS = unsupportedToggle,
            .GetPlayoutUnderrunCount = unsupported,
            .GetStats = [](webrtc_AudioDeviceModule_Stats_unique** stats, void*) -> int {
                if (stats) *stats = nullptr;
                return 0;
            },
            .GetPlayoutAudioParameters = [](webrtc_AudioParameters_unique** params, void*) -> int {
                if (params) *params = nullptr;
                return -1;
            },
            .GetRecordAudioParameters = [](webrtc_AudioParameters_unique** params, void*) -> int {
                if (!params) return -1;
                *params = webrtc_AudioParameters_new(sampleRate, channels, frameCount);
                return 0;
            },
            .OnDestroy = [](void*) {},
        };
        return webrtc_CreateAudioDeviceModuleWithCallback(&callbacks, this);
    }

    void submit(const std::int16_t* samples, std::size_t frames) {
        if (!samples || frames != frameCount) return;
        // Synchronize with unregister and StopRecording so the transport
        // cannot be used after WebRTC starts tearing down its audio state.
        const std::scoped_lock lock(mutex_);
        if (!recording_ || !transport_) return;
        std::uint32_t unusedLevel = 0;
        const std::int64_t captureTime = webrtc_TimeMillis() * 1'000'000;
        webrtc_AudioTransport_RecordedDataIsAvailable(transport_, samples, frames,
            sizeof(*samples) * channels, channels, sampleRate, 0, 0, 0, false, &unusedLevel, &captureTime);
    }

private:
    static constexpr std::uint32_t sampleRate = 48'000;
    static constexpr std::size_t channels = 2;
    static constexpr std::size_t frameCount = sampleRate / 100;

    static std::int32_t available(int* result, void*) { if (result) *result = 1; return 0; }
    static std::int32_t unavailable(int* result, void*) { if (result) *result = 0; return 0; }
    static std::int32_t deviceNameUnavailable(std::uint16_t, char*, char*, void*) { return -1; }
    static std::int32_t volumeUnavailable(std::uint32_t* volume, void*) { if (volume) *volume = 0; return -1; }
    static std::int32_t unsupportedVolume(std::uint32_t, void*) { return -1; }
    static std::int32_t unsupportedToggle(int, void*) { return -1; }
    static std::int32_t unsupported(void*) { return -1; }
    static std::int32_t noop(void*) { return 0; }
    static int no(void*) { return 0; }

    std::int32_t init() { initialized_ = true; return 0; }

    std::int32_t terminate() {
        const std::scoped_lock lock(mutex_);
        recording_ = false;
        recordingInitialized_ = false;
        initialized_ = false;
        return 0;
    }

    std::int32_t record(bool recording) {
        const std::scoped_lock lock(mutex_);
        recording_ = recording;
        return 0;
    }

    int recording() {
        const std::scoped_lock lock(mutex_);
        return recording_;
    }

    std::mutex mutex_;
    webrtc_AudioTransport* transport_ = nullptr;
    std::atomic_bool initialized_ = false;
    std::atomic_bool recordingInitialized_ = false;
    bool recording_ = false;
};

} // namespace

struct RtcHost::Session final : std::enable_shared_from_this<Session> {
    using WeakSession = std::weak_ptr<Session>;

    Session(const Callbacks& callbacks, HostMediaSource& media) : callbacks(callbacks), media(media) {}

    const Callbacks callbacks;
    HostMediaSource& media;
    LoopbackDevice audioDevice;
    std::atomic_bool active = true;
    std::atomic_bool failed = false;
    std::atomic_bool connected = false;
    std::atomic_bool inputOpen = false;
    std::atomic_uint64_t inputGeneration = 0;
    std::atomic_uint queuedInputMessages = 0;
    bool signalingStarted = false;
    bool localOfferSet = false;
    bool answerReceived = false;
    bool remoteDescriptionSet = false;
    bool statsPending = false;
    std::shared_ptr<SendQueuePressure> sendPressure = std::make_shared<SendQueuePressure>();
    std::vector<IceCandidate> pendingCandidates;
    std::string localOffer;

    Owned<webrtc_Thread_unique, webrtc_Thread_unique_delete> network;
    Owned<webrtc_Thread_unique, webrtc_Thread_unique_delete> signaling;
    Owned<webrtc_PeerConnectionFactoryInterface, webrtc_PeerConnectionFactoryInterface_Release> factory;
    Owned<webrtc_PeerConnectionInterface, webrtc_PeerConnectionInterface_Release> peer;
    Owned<webrtc_PeerConnectionObserver, webrtc_PeerConnectionObserver_delete> observer;
    Owned<webrtc_AdaptedVideoTrackSource, webrtc_AdaptedVideoTrackSource_Release> videoSource;
    Owned<webrtc_RtpSenderInterface, webrtc_RtpSenderInterface_Release> videoSender;
    Owned<webrtc_DataChannelInterface, webrtc_DataChannelInterface_Release> input;
    Owned<webrtc_DataChannelObserver, webrtc_DataChannelObserver_delete> inputObserver;
    Owned<webrtc_DataChannelInterface, webrtc_DataChannelInterface_Release> cursorChannel;
    Owned<webrtc_DataChannelObserver, webrtc_DataChannelObserver_delete> cursorObserver;
    quint64 cursorRequested = 0;
    bool cursorChannelAttempted = false;
    std::atomic<quint64> cursorTracked = 0;
    quint32 observedCursorAck = 0;
    std::chrono::steady_clock::time_point cursorProgress{};
    std::mutex cursorMutex;
    CursorOutbox cursorOutbox;

    template<class Function>
    void onSignalingThread(Function task) {
        webrtc_Thread_BlockingCall(webrtc_Thread_unique_get(signaling.get()), [](void* data) {
            (*static_cast<Function*>(data))();
        }, &task);
    }

    template<class Function>
    void post(Function task) {
        if (!active) return;
        callbacks.post([weak = weak_from_this(), task = std::move(task)]() mutable {
            if (const auto self = weak.lock(); self && self->active) task(*self);
        });
    }

    template<class... Args>
    void notify(std::function<void(Args...)> Callbacks::* member, Args... args) {
        if (!(callbacks.*member)) return;
        post([member, ...values = std::move(args)](Session& self) mutable {
            (self.callbacks.*member)(std::move(values)...);
        });
    }

    void reportError(std::string message) {
        if (!failed.exchange(true)) notify(&Callbacks::error, std::move(message));
    }

    std::string start(const std::vector<IceServer>& iceServers, bool relayOnly, bool withAudio, VideoSettings settings) {
        network.reset(webrtc_Thread_CreateWithSocketServer());
        signaling.reset(webrtc_Thread_Create());
        if (!network || !signaling || !webrtc_Thread_Start(webrtc_Thread_unique_get(network.get())) ||
            !webrtc_Thread_Start(webrtc_Thread_unique_get(signaling.get()))) {
            return "Could not start the native WebRTC threads.";
        }
        signalingStarted = true;

        std::string error;
        onSignalingThread([&] { error = createPeer(iceServers, relayOnly, settings); });
        if (!error.empty()) return error;

        const auto captureError = [weak = weak_from_this()](std::string message) {
            if (const auto self = weak.lock()) self->reportError(std::move(message));
        };
        if (media.supportsSeparateCursor()) {
            media.setCursorCallback([weak = weak_from_this()](quint64 epoch, CursorSnapshot state) {
                if (const auto self = weak.lock()) self->queueCursor(epoch, std::move(state));
            });
        }
        if (!media.startVideo(videoSource.get(), settings.maxFps, captureError, [weak = weak_from_this()](int width, int height) {
                if (const auto self = weak.lock()) self->notify(&Callbacks::videoSize, width, height);
            })) {
            return "Could not start desktop capture.";
        }
        if (!withAudio) {
            onSignalingThread([this] { createOffer(); });
        } else if (!media.startAudio(
                [this](const std::int16_t* samples, std::size_t frames) {
                    if (active && !failed) audioDevice.submit(samples, frames);
                },
                [weak = weak_from_this()](std::string message) {
                    if (const auto self = weak.lock()) {
                        self->post([available = message.empty()](Session& session) {
                            session.finishAudioStartup(available);
                        });
                    }
                },
                [weak = weak_from_this()](std::string message) {
                    if (const auto self = weak.lock()) self->notify(&Callbacks::audioError, std::move(message));
                })) {
            finishAudioStartup(false);
        }
        return {};
    }

    void finishAudioStartup(bool available) {
        if (failed) return;
        if (!available) notify(&Callbacks::audioUnavailable);
        onSignalingThread([this, available] {
            if (available) {
                const auto sharedStream = own<std_string_unique_delete>(std_string_new_from_bytes(streamId.data(), streamId.size()));
                if (auto error = addAudioTrack(std_string_unique_get(sharedStream.get())); !error.empty()) {
                    reportError(std::move(error));
                    return;
                }
            }
            createOffer();
        });
    }

    std::string createPeer(const std::vector<IceServer>& iceServers, bool relayOnly, VideoSettings settings) {
        const auto dependencies = own<webrtc_PeerConnectionFactoryDependencies_delete>(
            webrtc_PeerConnectionFactoryDependencies_new());
        webrtc_PeerConnectionFactoryDependencies_set_network_thread(dependencies.get(), webrtc_Thread_unique_get(network.get()));
        webrtc_PeerConnectionFactoryDependencies_set_signaling_thread(dependencies.get(), webrtc_Thread_unique_get(signaling.get()));

        auto* admRef = audioDevice.create();
        auto* audioEncoderRef = createOpusFactory();
        auto* audioDecoderRef = webrtc_CreateBuiltinAudioDecoderFactory();
        const auto adm = own<webrtc_AudioDeviceModule_Release>(webrtc_AudioDeviceModule_refcounted_get(admRef));
        const auto audioEncoder = own<webrtc_AudioEncoderFactory_Release>(webrtc_AudioEncoderFactory_refcounted_get(audioEncoderRef));
        const auto audioDecoder = own<webrtc_AudioDecoderFactory_Release>(webrtc_AudioDecoderFactory_refcounted_get(audioDecoderRef));
        if (!adm || !audioEncoder || !audioDecoder) return "Could not create the native audio device or codecs.";
        webrtc_PeerConnectionFactoryDependencies_set_adm(dependencies.get(), admRef);
        webrtc_PeerConnectionFactoryDependencies_set_audio_encoder_factory(dependencies.get(), audioEncoderRef);
        webrtc_PeerConnectionFactoryDependencies_set_audio_decoder_factory(dependencies.get(), audioDecoderRef);

        auto* videoEncoder = createFfmpegEncoderFactory(
            [weak = weak_from_this()](const QString& message) {
                if (const auto self = weak.lock()) self->reportError(message.toStdString());
            },
            [weak = weak_from_this()](const QString& backend) {
                if (const auto self = weak.lock()) self->notify(&Callbacks::encoder, backend.toStdString());
            }, sendPressure);
        if (!videoEncoder) return "Could not create the FFmpeg video encoder factory.";
        webrtc_PeerConnectionFactoryDependencies_set_video_encoder_factory(dependencies.get(), videoEncoder);
        webrtc_EnableMedia(dependencies.get());
        factory.reset(webrtc_PeerConnectionFactoryInterface_refcounted_get(
            webrtc_CreateModularPeerConnectionFactory(dependencies.get())));
        if (!factory) return "Could not create the native WebRTC factory.";

        static const webrtc_PeerConnectionObserver_cbs handlers{
            .OnSignalingChange = [](int, void*) {},
            .OnStandardizedIceConnectionChange = [](int, void*) {},
            .OnConnectionChange = [](int state, void* data) {
                static_cast<Session*>(data)->connectionChanged(state);
            },
            .OnIceCandidate = [](const webrtc_IceCandidate* candidate, void* data) {
                static_cast<Session*>(data)->sendCandidate(candidate);
            },
            .OnIceCandidateError = [](const char*, size_t, int, const char*, size_t, int, const char*, size_t, void*) {},
            .OnTrack = [](webrtc_RtpTransceiverInterface_refcounted* track, void*) {
                webrtc_RtpTransceiverInterface_Release(webrtc_RtpTransceiverInterface_refcounted_get(track));
            },
            .OnRemoveTrack = [](webrtc_RtpReceiverInterface_refcounted* receiver, void*) {
                webrtc_RtpReceiverInterface_Release(webrtc_RtpReceiverInterface_refcounted_get(receiver));
            },
            .OnDataChannel = [](webrtc_DataChannelInterface_refcounted* channel, void*) {
                const auto unexpected = own<webrtc_DataChannelInterface_Release>(webrtc_DataChannelInterface_refcounted_get(channel));
                if (unexpected) webrtc_DataChannelInterface_Close(unexpected.get());
            },
            .OnDestroy = [](void*) {},
            .OnIceGatheringChange = [](int, void*) {},
        };
        observer.reset(webrtc_PeerConnectionObserver_new(&handlers, this));
        const auto peerDependencies = own<webrtc_PeerConnectionDependencies_delete>(
            webrtc_PeerConnectionDependencies_new(observer.get()));
        const auto config = own<webrtc_PeerConnectionInterface_RTCConfiguration_delete>(
            webrtc_PeerConnectionInterface_RTCConfiguration_new());
        if (relayOnly) {
            webrtc_PeerConnectionInterface_RTCConfiguration_set_type(
                config.get(), webrtc_PeerConnectionInterface_IceTransportsType_kRelay);
        }
        webrtc_PeerConnectionInterface_RTCConfiguration_set_sdp_semantics(
            config.get(), webrtc_PeerConnectionInterface_SdpSemantics_kUnifiedPlan);
        auto* servers = webrtc_PeerConnectionInterface_RTCConfiguration_get_servers(config.get());
        for (const auto& server : iceServers) {
            if (server.urls.empty()) continue;
            const auto entry = own<webrtc_PeerConnectionInterface_IceServer_delete>(
                webrtc_PeerConnectionInterface_IceServer_new());
            auto* urls = webrtc_PeerConnectionInterface_IceServer_get_urls(entry.get());
            for (const auto& uri : server.urls) {
                const auto url = own<std_string_unique_delete>(std_string_new_from_bytes(uri.data(), uri.size()));
                std_string_vector_push_back(urls, std_string_unique_get(url.get()));
            }
            webrtc_PeerConnectionInterface_IceServer_set_username(entry.get(), server.username.data(), server.username.size());
            webrtc_PeerConnectionInterface_IceServer_set_password(entry.get(), server.credential.data(), server.credential.size());
            webrtc_PeerConnectionInterface_IceServer_vector_push_back(servers, entry.get());
        }
        webrtc_PeerConnectionInterface_refcounted* result = nullptr;
        webrtc_RTCError_unique* peerError = nullptr;
        webrtc_PeerConnectionFactoryInterface_CreatePeerConnectionOrError(factory.get(), config.get(), peerDependencies.get(), &result, &peerError);
        const auto detail = errorDetail(peerError);
        peer.reset(webrtc_PeerConnectionInterface_refcounted_get(result));
        if (!peer) return "Could not create a native WebRTC peer connection." + detail;

        const auto sharedStream = own<std_string_unique_delete>(std_string_new_from_bytes(streamId.data(), streamId.size()));
        if (auto error = addVideoTrack(std_string_unique_get(sharedStream.get()), settings); !error.empty()) return error;
        return createInputChannel();
    }

    std::string addVideoTrack(const std_string* sharedStream, VideoSettings settings) {
        auto* source = webrtc_AdaptedVideoTrackSource_Create();
        videoSource.reset(webrtc_AdaptedVideoTrackSource_refcounted_get(source));
        if (!videoSource) return "Could not create the desktop video source.";
        auto* sourceRef = webrtc_AdaptedVideoTrackSource_refcounted_cast_to_webrtc_VideoTrackSourceInterface(source);
        const auto ownedSource = own<webrtc_VideoTrackSourceInterface_Release>(webrtc_VideoTrackSourceInterface_refcounted_get(sourceRef));
        webrtc_VideoTrackInterface_refcounted* trackRef = nullptr;
        webrtc_PeerConnectionFactoryInterface_CreateVideoTrack(factory.get(), sourceRef, "screen", 6, &trackRef);
        const auto track = own<webrtc_VideoTrackInterface_Release>(webrtc_VideoTrackInterface_refcounted_get(trackRef));
        if (!track) return "Could not create the desktop video track.";

        auto* mediaTrackRef = webrtc_VideoTrackInterface_refcounted_cast_to_webrtc_MediaStreamTrackInterface(trackRef);
        const auto mediaTrack = own<webrtc_MediaStreamTrackInterface_Release>(webrtc_MediaStreamTrackInterface_refcounted_get(mediaTrackRef));
        const auto streams = own<std_string_vector_delete>(std_string_vector_new(0));
        std_string_vector_push_back(streams.get(), sharedStream);
        // AddTrack exposes the sender needed to update encoding parameters during the session.
        webrtc_RtpSenderInterface_refcounted* result = nullptr;
        webrtc_RTCError_unique* error = nullptr;
        webrtc_PeerConnectionInterface_AddTrack(peer.get(), mediaTrackRef, streams.get(), &result, &error);
        const auto detail = errorDetail(error);
        videoSender.reset(webrtc_RtpSenderInterface_refcounted_get(result));
        if (!videoSender) return "Could not publish the desktop video track." + detail;
        return setVideoParameters(settings);
    }

    std::string setVideoParameters(VideoSettings settings) {
        const auto parameters = own<webrtc_RtpParameters_delete>(webrtc_RtpSenderInterface_GetParameters(videoSender.get()));
        auto* encodings = webrtc_RtpParameters_get_encodings(parameters.get());
        if (webrtc_RtpEncodingParameters_vector_size(encodings) != 1) {
            return "The desktop video sender did not expose its single encoding.";
        }
        auto* encoding = webrtc_RtpEncodingParameters_vector_get(encodings, 0);
        const int bitrateBps = settings.maxBitrateMbps * 1'000'000;
        const double framerate = settings.maxFps;
        webrtc_RtpEncodingParameters_set_max_bitrate_bps(encoding, bitrateBps != 0, &bitrateBps);
        webrtc_RtpEncodingParameters_set_max_framerate(encoding, true, &framerate);
        // WebRTC tracks encoder overuse and video quality separately, then applies the
        // selected preference to the sink's frame-rate and resolution restrictions.
        int preference = webrtc_DegradationPreference_MAINTAIN_RESOLUTION;
        switch (settings.adaptation) {
        case VideoAdaptation::PreserveResolution: break;
        case VideoAdaptation::Balanced: preference = webrtc_DegradationPreference_BALANCED; break;
        case VideoAdaptation::PreserveFramerate: preference = webrtc_DegradationPreference_MAINTAIN_FRAMERATE; break;
        }
        webrtc_RtpParameters_set_degradation_preference(parameters.get(), true, &preference);
        webrtc_RTCError_unique* error = nullptr;
        webrtc_RtpSenderInterface_SetParameters(videoSender.get(), parameters.get(), &error);
        const auto detail = errorDetail(error);
        return detail.empty() ? std::string{} : "Could not set the desktop video parameters." + detail;
    }

    std::string addAudioTrack(const std_string* sharedStream) {
        const auto options = own<webrtc_AudioOptions_delete>(webrtc_AudioOptions_new());
        const int off = 0;
        webrtc_AudioOptions_set_echo_cancellation(options.get(), true, &off);
        webrtc_AudioOptions_set_auto_gain_control(options.get(), true, &off);
        webrtc_AudioOptions_set_noise_suppression(options.get(), true, &off);
        webrtc_AudioOptions_set_highpass_filter(options.get(), true, &off);
        webrtc_AudioSourceInterface_refcounted* sourceRef = nullptr;
        webrtc_PeerConnectionFactoryInterface_CreateAudioSource(factory.get(), options.get(), &sourceRef);
        const auto source = own<webrtc_AudioSourceInterface_Release>(webrtc_AudioSourceInterface_refcounted_get(sourceRef));
        if (!source) return "Could not create the system audio source.";
        webrtc_AudioTrackInterface_refcounted* trackRef = nullptr;
        webrtc_PeerConnectionFactoryInterface_CreateAudioTrack(factory.get(), sourceRef, "system-audio", 12, &trackRef);
        const auto audioTrack = own<webrtc_AudioTrackInterface_Release>(webrtc_AudioTrackInterface_refcounted_get(trackRef));
        if (!audioTrack) return "Could not create the system audio track.";
        auto* mediaTrackRef = webrtc_AudioTrackInterface_refcounted_cast_to_webrtc_MediaStreamTrackInterface(trackRef);
        const auto mediaTrack = own<webrtc_MediaStreamTrackInterface_Release>(webrtc_MediaStreamTrackInterface_refcounted_get(mediaTrackRef));
        const auto streams = own<std_string_vector_delete>(std_string_vector_new(0));
        std_string_vector_push_back(streams.get(), sharedStream);
        webrtc_RtpSenderInterface_refcounted* result = nullptr;
        webrtc_RTCError_unique* error = nullptr;
        webrtc_PeerConnectionInterface_AddTrack(peer.get(), mediaTrackRef, streams.get(), &result, &error);
        const auto detail = errorDetail(error);
        const auto sender = own<webrtc_RtpSenderInterface_Release>(webrtc_RtpSenderInterface_refcounted_get(result));
        return sender ? std::string{} : "Could not publish the system audio track." + detail;
    }

    std::string createInputChannel() {
        const auto init = own<webrtc_DataChannelInit_delete>(webrtc_DataChannelInit_new());
        // DataChannelInit defaults to unlimited retransmissions and in-band negotiation.
        webrtc_DataChannelInit_set_ordered(init.get(), true);
        webrtc_DataChannelInterface_refcounted* result = nullptr;
        webrtc_RTCError_unique* error = nullptr;
        webrtc_PeerConnectionInterface_CreateDataChannelOrError(peer.get(), inputLabel.data(), inputLabel.size(), init.get(), &result, &error);
        const auto detail = errorDetail(error);
        input.reset(webrtc_DataChannelInterface_refcounted_get(result));
        if (!input) return "Could not create the remote input channel." + detail;
        static const webrtc_DataChannelObserver_cbs handlers{
            .OnStateChange = [](void* data) { static_cast<Session*>(data)->updateInputState(); },
            .OnMessage = [](const std::uint8_t* bytes, std::size_t size, int binary, void* data) {
                if (!binary && bytes && size && size <= maxInputMessage) {
                    static_cast<Session*>(data)->onInputMessage(std::string(reinterpret_cast<const char*>(bytes), size));
                }
            },
            .OnDestroy = [](void*) {},
        };
        inputObserver.reset(webrtc_DataChannelObserver_new(&handlers, this));
        webrtc_DataChannelInterface_RegisterObserver(input.get(), inputObserver.get());
        updateInputState();
        return {};
    }

    void updateInputState() {
        const bool open = webrtc_DataChannelInterface_state(input.get()) == webrtc_DataChannelInterface_DataState_kOpen;
        if (inputOpen == open) return;
        ++inputGeneration;
        inputOpen = open;
        notify(&Callbacks::inputOpen, open);
    }

    void connectionChanged(int state) {
        const bool nowConnected = state == webrtc_PeerConnectionInterface_PeerConnectionState_kConnected;
        if (connected != nowConnected) {
            ++inputGeneration;
            connected = nowConnected;
        }
        notify(&Callbacks::connectionState, std::string(connectionStateName(state)));
    }

    void onInputMessage(std::string message) {
        if (!active || failed || !connected || !inputOpen || !callbacks.inputMessage) return;
        if (queuedInputMessages.fetch_add(1) >= maxQueuedInputMessages) {
            --queuedInputMessages;
            reportError("The remote input queue overflowed.");
            return;
        }
        post([generation = inputGeneration.load(), message = std::move(message)](Session& self) mutable {
            --self.queuedInputMessages;
            if (!self.failed && self.connected && self.inputOpen && self.inputGeneration == generation) {
                self.callbacks.inputMessage(std::move(message));
            }
        });
    }

    bool sendInput(std::string_view message) {
        return !failed && connected && inputOpen && !message.empty() && message.size() <= maxInputMessage &&
            webrtc_DataChannelInterface_Send(input.get(), reinterpret_cast<const std::uint8_t*>(message.data()),
                                             message.size(), false);
    }

    void setSeparateCursor(quint64 epoch) {
        if (epoch && (failed || !connected || !inputOpen || !media.supportsSeparateCursor())) return;
        if (cursorRequested == epoch) return;
        cursorRequested = epoch;
        {
            const std::lock_guard lock(cursorMutex);
            cursorOutbox.reset();
        }
        cursorProgress = std::chrono::steady_clock::now();
        if (epoch && !cursorChannelAttempted) {
            cursorChannelAttempted = true;
            createCursorChannel();
        }
        updateCursorTracking();
    }

    void createCursorChannel() {
        const auto init = own<webrtc_DataChannelInit_delete>(webrtc_DataChannelInit_new());
        if (!init) return;
        webrtc_DataChannelInit_set_ordered(init.get(), true);
        webrtc_DataChannelInterface_refcounted* result = nullptr;
        webrtc_RTCError_unique* error = nullptr;
        webrtc_PeerConnectionInterface_CreateDataChannelOrError(
            peer.get(), cursorChannelLabel, sizeof(cursorChannelLabel) - 1, init.get(), &result, &error);
        const auto ignored = own<webrtc_RTCError_unique_delete>(error);
        cursorChannel.reset(webrtc_DataChannelInterface_refcounted_get(result));
        if (!cursorChannel) return;
        static const webrtc_DataChannelObserver_cbs handlers{
            .OnStateChange = [](void* data) { static_cast<Session*>(data)->updateCursorTracking(); },
            .OnMessage = [](const std::uint8_t* bytes, std::size_t size, int binary, void* data) {
                if (binary && bytes && size == sizeof(quint32)) {
                    auto& self = *static_cast<Session*>(data);
                    const auto seq = decodeCursorAck(QByteArray::fromRawData(reinterpret_cast<const char*>(bytes), size));
                    bool advanced = false;
                    {
                        const std::lock_guard lock(self.cursorMutex);
                        advanced = self.cursorOutbox.acknowledge(seq);
                    }
                    if (advanced) self.scheduleCursorFlush();
                }
            },
            .OnDestroy = [](void*) {},
        };
        cursorObserver.reset(webrtc_DataChannelObserver_new(&handlers, this));
        webrtc_DataChannelInterface_RegisterObserver(cursorChannel.get(), cursorObserver.get());
    }

    void updateCursorTracking() {
        const bool open = cursorChannel &&
            webrtc_DataChannelInterface_state(cursorChannel.get()) == webrtc_DataChannelInterface_DataState_kOpen;
        const auto epoch = active && !failed && connected && inputOpen && open ? cursorRequested : 0;
        if (cursorTracked.exchange(epoch) == epoch) return;
        post([epoch](Session& self) {
            if (self.cursorTracked == epoch) self.media.setCursorEpoch(epoch);
        });
    }

    void queueCursor(quint64 epoch, CursorSnapshot state) {
        if (!active || failed || cursorTracked != epoch) return;
        {
            const std::lock_guard lock(cursorMutex);
            if (cursorTracked != epoch) return;
            cursorOutbox.observe(epoch, std::move(state));
        }
        scheduleCursorFlush();
    }

    void scheduleCursorFlush() {
        {
            const std::lock_guard lock(cursorMutex);
            if (!cursorOutbox.schedule()) return;
        }
        post([](Session& self) {
            self.onSignalingThread([&] { self.flushCursor(); });
        });
    }

    void flushCursor() {
        const auto update = [&] {
            const std::lock_guard lock(cursorMutex);
            if (!cursorOutbox.inFlight()) cursorProgress = std::chrono::steady_clock::now();
            return cursorOutbox.take(connected && inputOpen && !failed ? cursorTracked.load() : 0);
        }();
        if (!update || !cursorChannel) return;
        if (!update->sequence) {
            closeCursorChannel();
            return;
        }
        const auto packet = encodeCursor(update->state, update->epoch, update->sequence, update->includeShape);
        if (!webrtc_DataChannelInterface_Send(cursorChannel.get(),
                reinterpret_cast<const std::uint8_t*>(packet.constData()), packet.size(), true)) {
            closeCursorChannel();
        }
    }

    void closeCursorChannel() {
        cursorRequested = 0;
        updateCursorTracking();
        if (cursorChannel) webrtc_DataChannelInterface_Close(cursorChannel.get());
    }

    void checkCursorProgress() {
        if (!cursorTracked) return;
        const auto [ack, inFlight] = [&] {
            const std::lock_guard lock(cursorMutex);
            return std::pair{cursorOutbox.acknowledged(), cursorOutbox.inFlight()};
        }();
        if (ack != observedCursorAck) {
            observedCursorAck = ack;
            cursorProgress = std::chrono::steady_clock::now();
        } else if (inFlight && std::chrono::steady_clock::now() - cursorProgress > std::chrono::seconds(3)) {
            closeCursorChannel();
        }
    }

    bool setVideoSettings(VideoSettings settings) {
        return !failed && connected && setVideoParameters(settings).empty();
    }

    void createOffer() {
        if (failed) return;
        static const webrtc_CreateSessionDescriptionObserver_cbs handlers{
            .OnSuccess = [](webrtc_SessionDescriptionInterface_unique* description, void* data) {
                auto offer = own<webrtc_SessionDescriptionInterface_unique_delete>(description);
                if (const auto self = static_cast<WeakSession*>(data)->lock(); self && self->active && !self->failed) {
                    self->setOffer(offer.release());
                }
            },
            .OnFailure = [](webrtc_RTCError_unique* error, void* data) {
                const auto detail = errorDetail(error);
                if (const auto self = static_cast<WeakSession*>(data)->lock()) {
                    self->reportError("Could not create the WebRTC offer." + detail);
                }
            },
            .OnDestroy = [](void* data) { delete static_cast<WeakSession*>(data); },
        };
        const auto options = own<webrtc_PeerConnectionInterface_RTCOfferAnswerOptions_delete>(
            webrtc_PeerConnectionInterface_RTCOfferAnswerOptions_new());
        // Offer the local audio and video tracks as send-only.
        webrtc_PeerConnectionInterface_RTCOfferAnswerOptions_set_offer_to_receive_audio(options.get(), 0);
        webrtc_PeerConnectionInterface_RTCOfferAnswerOptions_set_offer_to_receive_video(options.get(), 0);
        const auto result = own<webrtc_CreateSessionDescriptionObserver_Release>(
            webrtc_CreateSessionDescriptionObserver_make_ref_counted(&handlers, new WeakSession(weak_from_this())));
        webrtc_PeerConnectionInterface_CreateOffer(peer.get(), result.get(), options.get());
    }

    void setOffer(webrtc_SessionDescriptionInterface_unique* description) {
        auto offer = own<webrtc_SessionDescriptionInterface_unique_delete>(description);
        std_string_unique* text = nullptr;
        const bool serialized = webrtc_SessionDescriptionInterface_ToString(webrtc_SessionDescriptionInterface_unique_get(offer.get()), &text);
        localOffer = takeString(text);
        if (!serialized || localOffer.empty()) {
            reportError("Could not serialize the WebRTC offer.");
            return;
        }
        static const webrtc_SetLocalDescriptionObserverInterface_cbs handlers{
            .OnSetLocalDescriptionComplete = [](webrtc_RTCError_unique* error, void* data) {
                const auto detail = errorDetail(error);
                if (const auto self = static_cast<WeakSession*>(data)->lock(); self && self->active && !self->failed) {
                    if (detail.empty()) {
                        self->localOfferSet = true;
                        self->notify(&Callbacks::localOffer, std::move(self->localOffer));
                    } else {
                        self->reportError("Could not set the local WebRTC offer." + detail);
                    }
                }
            },
            .OnDestroy = [](void* data) { delete static_cast<WeakSession*>(data); },
        };
        auto* result = webrtc_SetLocalDescriptionObserverInterface_make_ref_counted(&handlers, new WeakSession(weak_from_this()));
        webrtc_PeerConnectionInterface_SetLocalDescription(peer.get(), offer.release(), result);
        webrtc_SetLocalDescriptionObserverInterface_Release(webrtc_SetLocalDescriptionObserverInterface_refcounted_get(result));
    }

    void acceptAnswer(const std::string& sdp) {
        if (failed) return;
        if (!localOfferSet || std::exchange(answerReceived, true) || sdp.empty()) {
            reportError("The viewer sent an unexpected WebRTC answer.");
            return;
        }
        auto* answer = webrtc_CreateSessionDescription(webrtc_SdpType_kAnswer, sdp.data(), sdp.size());
        if (!answer) {
            reportError("Could not parse the viewer's WebRTC answer.");
            return;
        }
        static const webrtc_SetRemoteDescriptionObserverInterface_cbs handlers{
            .OnSetRemoteDescriptionComplete = [](webrtc_RTCError_unique* error, void* data) {
                const auto detail = errorDetail(error);
                if (const auto self = static_cast<WeakSession*>(data)->lock(); self && self->active && !self->failed) {
                    if (detail.empty()) {
                        self->remoteDescriptionSet = true;
                        for (const auto& candidate : self->pendingCandidates) self->applyCandidate(candidate);
                        self->pendingCandidates.clear();
                    } else {
                        self->reportError("Could not accept the viewer's WebRTC answer." + detail);
                    }
                }
            },
            .OnDestroy = [](void* data) { delete static_cast<WeakSession*>(data); },
        };
        auto* result = webrtc_SetRemoteDescriptionObserverInterface_make_ref_counted(&handlers, new WeakSession(weak_from_this()));
        webrtc_PeerConnectionInterface_SetRemoteDescription(peer.get(), answer, result);
        webrtc_SetRemoteDescriptionObserverInterface_Release(webrtc_SetRemoteDescriptionObserverInterface_refcounted_get(result));
    }

    void addCandidate(IceCandidate candidate) {
        if (failed || candidate.candidate.empty()) return;
        if (remoteDescriptionSet) {
            applyCandidate(candidate);
        } else if (pendingCandidates.size() < maxPendingCandidates) {
            pendingCandidates.push_back(std::move(candidate));
        } else {
            reportError("The viewer sent too many ICE candidates before answering.");
        }
    }

    void applyCandidate(const IceCandidate& candidate) {
        webrtc_SdpParseError_unique* error = nullptr;
        const auto parsed = own<webrtc_IceCandidate_delete>(webrtc_CreateIceCandidate(
            candidate.sdpMid.data(), candidate.sdpMid.size(), candidate.sdpMLineIndex,
            candidate.candidate.data(), candidate.candidate.size(), &error));
        const auto ownedError = own<webrtc_SdpParseError_unique_delete>(error);
        if (parsed) webrtc_PeerConnectionInterface_AddIceCandidate(peer.get(), parsed.get());
    }

    void sendCandidate(const webrtc_IceCandidate* candidate) {
        if (!active || failed || !callbacks.localCandidate || !candidate) return;
        std_string_unique* sdp = nullptr;
        const bool serialized = webrtc_IceCandidate_ToString(candidate, &sdp);
        auto text = takeString(sdp);
        if (!serialized || text.empty()) return;
        std_string_unique* mid = nullptr;
        webrtc_IceCandidate_sdp_mid(candidate, &mid);
        notify(&Callbacks::localCandidate, IceCandidate{
            std::move(text), takeString(mid), webrtc_IceCandidate_sdp_mline_index(candidate)});
    }

    void pollStats() {
        checkCursorProgress();
        if (!callbacks.routeStats || failed || std::exchange(statsPending, true)) return;
        static webrtc_RTCStatsCollectorCallback_cbs handlers{
            .OnStatsDelivered = [](const webrtc_RTCStatsReport_refcounted* report, void* data) {
                const auto owned = own<webrtc_RTCStatsReport_Release>(webrtc_RTCStatsReport_refcounted_get_const(report));
                if (const auto self = static_cast<WeakSession*>(data)->lock(); self && self->active && !self->failed) {
                    self->statsPending = false;
                    const auto text = takeString(webrtc_RTCStatsReport_ToJson(owned.get()));
                    const auto reports = QJsonDocument::fromJson(QByteArray::fromStdString(text)).array();
                    self->sendPressure->observe(reports, SendQueuePressure::Clock::now());
                    if (videoTiming().isDebugEnabled()) {
                        for (const auto& entry : reports) {
                            const auto stats = entry.toObject();
                            if (stats.value("kind") != "video") continue;
                            if (stats.value("type") == "outbound-rtp")
                                qCDebug(videoTiming) << "sender" << "encoded" << stats.value("framesEncoded")
                                    << "sent" << stats.value("framesSent") << "qp-sum" << stats.value("qpSum")
                                    << "limitation" << stats.value("qualityLimitationReason")
                                    << "packet-send-delay-sum-s" << stats.value("totalPacketSendDelay")
                                    << "packets" << stats.value("packetsSent")
                                    << "pacing-drops" << self->sendPressure->pacingDrops();
                            if (stats.value("type") == "media-source")
                                qCDebug(videoTiming) << "source" << "frames" << stats.value("frames")
                                                     << "fps" << stats.value("framesPerSecond");
                        }
                    }
                    self->notify(&Callbacks::routeStats, findRoute(reports));
                }
            },
            .OnDestroy = [](void* data) { delete static_cast<WeakSession*>(data); },
        };
        webrtc_PeerConnectionInterface_GetStats(peer.get(), &handlers, new WeakSession(weak_from_this()));
    }

    void close() {
        active = false;
        media.stop();
        if (signalingStarted) {
            onSignalingThread([this] {
                if (cursorChannel) {
                    if (cursorObserver) webrtc_DataChannelInterface_UnregisterObserver(cursorChannel.get());
                    webrtc_DataChannelInterface_Close(cursorChannel.get());
                    cursorChannel.reset();
                    cursorObserver.reset();
                }
                if (input) {
                    if (inputObserver) webrtc_DataChannelInterface_UnregisterObserver(input.get());
                    webrtc_DataChannelInterface_Close(input.get());
                    input.reset();
                    inputObserver.reset();
                }
                if (peer) {
                    webrtc_PeerConnectionInterface_Close(peer.get());
                    videoSender.reset();
                    peer.reset();
                }
                observer.reset();
                videoSource.reset();
                factory.reset();
            });
        }
        signaling.reset();
        network.reset();
    }
};

RtcHost::RtcHost(Callbacks callbacks, std::unique_ptr<HostMediaSource> media)
    : callbacks_(std::move(callbacks)), media_(media ? std::move(media) : std::make_unique<WindowsHostMedia>()) {}

RtcHost::~RtcHost() {
    close();
    if (initialized_) rtc::releaseRuntime();
}

bool RtcHost::start(const std::vector<IceServer>& iceServers, bool relayOnly, bool withAudio, VideoSettings settings) {
    close();
    if (!settings.valid()) {
        if (callbacks_.error) callbacks_.error("The desktop video parameters are invalid.");
        return false;
    }
    if (!callbacks_.post) {
        if (callbacks_.error) callbacks_.error("The WebRTC owner thread dispatcher is missing.");
        return false;
    }
    if (!initialized_) initialized_ = rtc::acquireRuntime();
    if (!initialized_) {
        if (callbacks_.error) callbacks_.error("Could not initialize native WebRTC.");
        return false;
    }
    auto session = std::make_shared<Session>(callbacks_, *media_);
    if (auto error = session->start(iceServers, relayOnly, withAudio, settings); !error.empty()) {
        session->close();
        if (callbacks_.error) callbacks_.error(std::move(error));
        return false;
    }
    session_ = std::move(session);
    return true;
}

void RtcHost::acceptAnswer(std::string sdp) {
    if (session_) session_->onSignalingThread([&] { session_->acceptAnswer(sdp); });
}

void RtcHost::addRemoteCandidate(IceCandidate candidate) {
    if (session_) session_->onSignalingThread([&] { session_->addCandidate(std::move(candidate)); });
}

bool RtcHost::sendInput(std::string_view message) {
    bool sent = false;
    if (session_) session_->onSignalingThread([&] { sent = session_->sendInput(message); });
    return sent;
}

void RtcHost::offerSeparateCursor(quint64 epoch) {
    if (media_->supportsSeparateCursor()) sendInput("cursor-offer:" + std::to_string(epoch));
}

void RtcHost::setSeparateCursor(quint64 epoch) {
    if (session_) session_->onSignalingThread([&] { session_->setSeparateCursor(epoch); });
}

void RtcHost::noteCursorMove(quint64 epoch, quint32 serial, QPointF position) {
    if (session_ && session_->cursorTracked == epoch) media_->noteCursorMove(epoch, serial, position);
}

bool RtcHost::setVideoSettings(VideoSettings settings) {
    if (!session_ || !settings.valid()) return false;
    bool applied = false;
    session_->onSignalingThread([&] { applied = session_->setVideoSettings(settings); });
    if (applied) media_->setVideoMaxFps(settings.maxFps);
    return applied;
}

void RtcHost::pollStats() {
    if (session_) session_->onSignalingThread([&] { session_->pollStats(); });
}

void RtcHost::close() {
    if (const auto session = std::exchange(session_, nullptr)) session->close();
}

} // namespace evp
