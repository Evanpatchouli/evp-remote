#include "rtc_viewer.h"
#include "../media/webrtc_ffmpeg_decoder.h"

#include <winsock2.h>
#include <webrtc_c.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>

#include <atomic>
#include <mutex>
#include <utility>

namespace evp {
namespace {

template<auto Delete>
struct Deleter {
    template<class T>
    void operator()(T* ptr) const { Delete(ptr); }
};

template<auto Delete, class T>
auto own(T* ptr) {
    return std::unique_ptr<T, Deleter<Delete>>(ptr);
}

std::string takeString(std_string_unique* value) {
    const auto owner = own<std_string_unique_delete>(value);
    return value ? std_string_c_str(std_string_unique_get(value)) : "";
}

std::string readError(webrtc_RTCError_unique* error) {
    if (!error) return {};
    const auto* value = webrtc_RTCError_unique_get(error);
    if (webrtc_RTCError_ok(value)) return {};
    const char* message = nullptr;
    std::size_t size = 0;
    webrtc_RTCError_message(value, &message, &size);
    return message && size ? std::string(message, size) : "Unknown WebRTC error.";
}

std::mutex runtimeMutex;
unsigned runtimeUsers = 0;

bool acquireRuntime() {
    const std::scoped_lock lock(runtimeMutex);
    if (runtimeUsers == 0) {
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return false;
        webrtc_InitializeSSL();
    }
    ++runtimeUsers;
    return true;
}

void releaseRuntime() {
    const std::scoped_lock lock(runtimeMutex);
    if (--runtimeUsers == 0) WSACleanup();
}

const char* connectionStateName(int state) {
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kNew) return "new";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kConnecting) return "connecting";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kConnected) return "connected";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kDisconnected) return "disconnected";
    if (state == webrtc_PeerConnectionInterface_PeerConnectionState_kClosed) return "closed";
    return "failed";
}

bool candidateType(const QString& type) {
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
        if (candidateType(local) && candidateType(remote)) {
            return {local == "relay" || remote == "relay" ? "relay" : "direct",
                    local.toStdString(), remote.toStdString()};
        }
    }
    return {};
}

constexpr std::size_t maxPendingCandidates = 256;
constexpr std::size_t maxInputMessage = 256;

} // namespace

struct RtcViewer::Session final : std::enable_shared_from_this<Session> {
    explicit Session(const Callbacks& callbacks) : callbacks(callbacks) {}

    const Callbacks callbacks;
    std::atomic_bool active = true;
    std::atomic_bool audioReceived = false;
    std::atomic_bool playoutReceived = false;
    bool offerReceived = false;
    bool remoteDescriptionSet = false;
    bool statsPending = false;
    bool inputOpen = false;
    std::vector<IceCandidate> pendingCandidates;
    std::string localAnswer;

    std::unique_ptr<webrtc_Thread_unique, Deleter<webrtc_Thread_unique_delete>> network;
    std::unique_ptr<webrtc_Thread_unique, Deleter<webrtc_Thread_unique_delete>> signaling;
    std::unique_ptr<webrtc_PeerConnectionFactoryInterface, Deleter<webrtc_PeerConnectionFactoryInterface_Release>> factory;
    std::unique_ptr<webrtc_PeerConnectionInterface, Deleter<webrtc_PeerConnectionInterface_Release>> peer;
    std::unique_ptr<webrtc_PeerConnectionObserver, Deleter<webrtc_PeerConnectionObserver_delete>> observer;
    std::unique_ptr<webrtc_DataChannelInterface, Deleter<webrtc_DataChannelInterface_Release>> input;
    std::unique_ptr<webrtc_DataChannelObserver, Deleter<webrtc_DataChannelObserver_delete>> inputObserver;
    std::unique_ptr<webrtc_VideoTrackInterface, Deleter<webrtc_VideoTrackInterface_Release>> video;
    std::unique_ptr<webrtc_VideoSinkInterface, Deleter<webrtc_VideoSinkInterface_delete>> videoSink;
    std::unique_ptr<webrtc_AudioTrackInterface, Deleter<webrtc_AudioTrackInterface_Release>> audio;
    std::unique_ptr<webrtc_AudioTrackSinkInterface, Deleter<webrtc_AudioTrackSinkInterface_delete>> audioSink;

    template<class Function>
    void onSignalingThread(Function task) {
        webrtc_Thread_BlockingCall(webrtc_Thread_unique_get(signaling.get()), [](void* data) {
            (*static_cast<Function*>(data))();
        }, &task);
    }

    template<class Function>
    void post(Function task) {
        if (!active) return;
        callbacks.post([weak = weak_from_this(), task = std::move(task)] {
            if (const auto self = weak.lock(); self && self->active) task(*self);
        });
    }

    void reportError(std::string message) {
        post([message = std::move(message)](Session& self) {
            if (self.callbacks.error) self.callbacks.error(message);
        });
    }

    std::string start(const std::vector<IceServer>& iceServers, bool relayOnly, ID3D11Device* device) {
        network.reset(webrtc_Thread_CreateWithSocketServer());
        signaling.reset(webrtc_Thread_Create());
        if (!network || !signaling || !webrtc_Thread_Start(webrtc_Thread_unique_get(network.get())) ||
            !webrtc_Thread_Start(webrtc_Thread_unique_get(signaling.get()))) {
            return "Could not start the native WebRTC threads.";
        }

        auto dependencies = own<webrtc_PeerConnectionFactoryDependencies_delete>(
            webrtc_PeerConnectionFactoryDependencies_new());
        webrtc_PeerConnectionFactoryDependencies_set_network_thread(dependencies.get(), webrtc_Thread_unique_get(network.get()));
        webrtc_PeerConnectionFactoryDependencies_set_signaling_thread(dependencies.get(), webrtc_Thread_unique_get(signaling.get()));
        auto* audioEncoder = webrtc_CreateBuiltinAudioEncoderFactory();
        auto* audioDecoder = webrtc_CreateBuiltinAudioDecoderFactory();
        webrtc_PeerConnectionFactoryDependencies_set_audio_encoder_factory(dependencies.get(), audioEncoder);
        webrtc_PeerConnectionFactoryDependencies_set_audio_decoder_factory(dependencies.get(), audioDecoder);
        webrtc_AudioEncoderFactory_Release(webrtc_AudioEncoderFactory_refcounted_get(audioEncoder));
        webrtc_AudioDecoderFactory_Release(webrtc_AudioDecoderFactory_refcounted_get(audioDecoder));
        auto* videoDecoder = makeFfmpegDecoderFactory(device);
        if (!videoDecoder) return "Could not create the FFmpeg video decoder factory.";
        webrtc_PeerConnectionFactoryDependencies_set_video_decoder_factory(dependencies.get(), videoDecoder);
        webrtc_EnableMedia(dependencies.get());
        factory.reset(webrtc_PeerConnectionFactoryInterface_refcounted_get(
            webrtc_CreateModularPeerConnectionFactory(dependencies.get())));
        if (!factory) return "Could not create the native WebRTC factory.";

        auto config = own<webrtc_PeerConnectionInterface_RTCConfiguration_delete>(
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

        static const webrtc_PeerConnectionObserver_cbs handlers{
            .OnSignalingChange = [](int, void*) {},
            .OnStandardizedIceConnectionChange = [](int, void*) {},
            .OnConnectionChange = [](int state, void* data) { static_cast<Session*>(data)->connectionChanged(state); },
            .OnIceCandidate = [](const webrtc_IceCandidate* candidate, void* data) { static_cast<Session*>(data)->localCandidate(candidate); },
            .OnIceCandidateError = [](const char*, size_t, int, const char*, size_t, int, const char*, size_t, void*) {},
            .OnTrack = [](webrtc_RtpTransceiverInterface_refcounted* transceiver, void* data) {
                static_cast<Session*>(data)->attachTrack(transceiver);
            },
            .OnRemoveTrack = [](webrtc_RtpReceiverInterface_refcounted* receiver, void*) {
                webrtc_RtpReceiverInterface_Release(webrtc_RtpReceiverInterface_refcounted_get(receiver));
            },
            .OnDataChannel = [](webrtc_DataChannelInterface_refcounted* channel, void* data) { static_cast<Session*>(data)->attachInput(channel); },
            .OnDestroy = [](void*) {},
            .OnIceGatheringChange = [](int, void*) {},
        };
        observer.reset(webrtc_PeerConnectionObserver_new(&handlers, this));
        auto peerDependencies = own<webrtc_PeerConnectionDependencies_delete>(webrtc_PeerConnectionDependencies_new(observer.get()));
        webrtc_PeerConnectionInterface_refcounted* result = nullptr;
        webrtc_RTCError_unique* error = nullptr;
        webrtc_PeerConnectionFactoryInterface_CreatePeerConnectionOrError(factory.get(), config.get(), peerDependencies.get(), &result, &error);
        const auto ownedError = own<webrtc_RTCError_unique_delete>(error);
        peer.reset(webrtc_PeerConnectionInterface_refcounted_get(result));
        if (peer) return {};
        auto errorMessage = std::string("Could not create a native WebRTC peer connection.");
        if (const auto detail = readError(error); !detail.empty()) errorMessage += " " + detail;
        return errorMessage;
    }

    void acceptOffer(const std::string& sdp) {
        if (std::exchange(offerReceived, true) || sdp.empty()) {
            reportError("The host sent an unexpected WebRTC offer.");
            return;
        }
        auto* offer = webrtc_CreateSessionDescription(webrtc_SdpType_kOffer, sdp.data(), sdp.size());
        if (!offer) {
            reportError("Could not parse the host's WebRTC offer.");
            return;
        }
        static const webrtc_SetRemoteDescriptionObserverInterface_cbs handlers{
            .OnSetRemoteDescriptionComplete = [](webrtc_RTCError_unique* error, void* data) {
                const auto owned = own<webrtc_RTCError_unique_delete>(error);
                auto& self = *static_cast<Session*>(data);
                if (!self.active) return;
                if (const auto detail = readError(error); !detail.empty()) {
                    self.reportError("Could not accept the host's WebRTC offer. " + detail);
                } else {
                    self.createAnswer();
                }
            },
            .OnDestroy = [](void*) {},
        };
        auto* result = webrtc_SetRemoteDescriptionObserverInterface_make_ref_counted(&handlers, this);
        webrtc_PeerConnectionInterface_SetRemoteDescription(peer.get(), offer, result);
        webrtc_SetRemoteDescriptionObserverInterface_Release(webrtc_SetRemoteDescriptionObserverInterface_refcounted_get(result));
    }

    void createAnswer() {
        remoteDescriptionSet = true;
        for (const auto& candidate : pendingCandidates) applyCandidate(candidate);
        pendingCandidates.clear();
        static const webrtc_CreateSessionDescriptionObserver_cbs handlers{
            .OnSuccess = [](webrtc_SessionDescriptionInterface_unique* answer, void* data) {
                auto& self = *static_cast<Session*>(data);
                if (self.active) self.setAnswer(answer);
                else webrtc_SessionDescriptionInterface_unique_delete(answer);
            },
            .OnFailure = [](webrtc_RTCError_unique* error, void* data) {
                const auto owned = own<webrtc_RTCError_unique_delete>(error);
                static_cast<Session*>(data)->reportError("Could not create a WebRTC answer. " + readError(error));
            },
            .OnDestroy = [](void*) {},
        };
        auto* result = webrtc_CreateSessionDescriptionObserver_make_ref_counted(&handlers, this);
        webrtc_PeerConnectionInterface_CreateAnswer(peer.get(), result, nullptr);
        webrtc_CreateSessionDescriptionObserver_Release(result);
    }

    void setAnswer(webrtc_SessionDescriptionInterface_unique* answer) {
        std_string_unique* text = nullptr;
        if (!webrtc_SessionDescriptionInterface_ToString(webrtc_SessionDescriptionInterface_unique_get(answer), &text)) {
            webrtc_SessionDescriptionInterface_unique_delete(answer);
            reportError("Could not serialize the WebRTC answer.");
            return;
        }
        localAnswer = takeString(text);
        static const webrtc_SetLocalDescriptionObserverInterface_cbs handlers{
            .OnSetLocalDescriptionComplete = [](webrtc_RTCError_unique* error, void* data) {
                const auto owned = own<webrtc_RTCError_unique_delete>(error);
                auto& self = *static_cast<Session*>(data);
                if (!self.active) return;
                if (const auto detail = readError(error); !detail.empty()) {
                    self.reportError("Could not set the local WebRTC answer. " + detail);
                } else if (self.callbacks.localAnswer) {
                    self.callbacks.localAnswer(std::move(self.localAnswer));
                }
            },
            .OnDestroy = [](void*) {},
        };
        auto* result = webrtc_SetLocalDescriptionObserverInterface_make_ref_counted(&handlers, this);
        webrtc_PeerConnectionInterface_SetLocalDescription(peer.get(), answer, result);
        webrtc_SetLocalDescriptionObserverInterface_Release(webrtc_SetLocalDescriptionObserverInterface_refcounted_get(result));
    }

    void addCandidate(IceCandidate candidate) {
        if (candidate.candidate.empty()) return;
        if (remoteDescriptionSet) {
            applyCandidate(candidate);
        } else if (pendingCandidates.size() < maxPendingCandidates) {
            pendingCandidates.push_back(std::move(candidate));
        } else {
            reportError("The host sent too many ICE candidates before its offer.");
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

    void localCandidate(const webrtc_IceCandidate* candidate) {
        if (!active || !callbacks.localCandidate || !candidate) return;
        std_string_unique* sdp = nullptr;
        if (!webrtc_IceCandidate_ToString(candidate, &sdp)) return;
        std_string_unique* mid = nullptr;
        webrtc_IceCandidate_sdp_mid(candidate, &mid);
        callbacks.localCandidate({takeString(sdp), takeString(mid),
                                  webrtc_IceCandidate_sdp_mline_index(candidate)});
    }

    void connectionChanged(int state) {
        if (active && callbacks.connectionState) callbacks.connectionState(connectionStateName(state));
    }

    void attachInput(webrtc_DataChannelInterface_refcounted* ref) {
        auto channel = own<webrtc_DataChannelInterface_Release>(
            webrtc_DataChannelInterface_refcounted_get(ref));
        if (!channel || !active) return;
        // The project host opens "input" ordered with unlimited retransmission. This
        // SDK exposes its label and state but no DCEP reliability query for the receiver.
        if (input || takeString(webrtc_DataChannelInterface_label(channel.get())) != "input") {
            webrtc_DataChannelInterface_Close(channel.get());
            return;
        }
        static const webrtc_DataChannelObserver_cbs handlers{
            .OnStateChange = [](void* data) { static_cast<Session*>(data)->updateInputState(); },
            .OnMessage = [](const std::uint8_t* bytes, std::size_t size, int binary, void* data) {
                auto& self = *static_cast<Session*>(data);
                if (self.active && !binary && bytes && size && size <= maxInputMessage && self.callbacks.inputMessage) {
                    self.callbacks.inputMessage(std::string(reinterpret_cast<const char*>(bytes), size));
                }
            },
            .OnDestroy = [](void*) {},
        };
        input = std::move(channel);
        inputObserver.reset(webrtc_DataChannelObserver_new(&handlers, this));
        webrtc_DataChannelInterface_RegisterObserver(input.get(), inputObserver.get());
        updateInputState();
    }

    void updateInputState() {
        if (!active || !input) return;
        const bool open = webrtc_DataChannelInterface_state(input.get()) == webrtc_DataChannelInterface_DataState_kOpen;
        if (std::exchange(inputOpen, open) != open && callbacks.inputOpen) callbacks.inputOpen(open);
    }

    bool sendInput(std::string_view message) {
        return inputOpen && input && !message.empty() && message.size() <= maxInputMessage &&
            webrtc_DataChannelInterface_Send(input.get(), reinterpret_cast<const std::uint8_t*>(message.data()),
                                             message.size(), false);
    }

    void attachTrack(webrtc_RtpTransceiverInterface_refcounted* ref) {
        const auto transceiver = own<webrtc_RtpTransceiverInterface_Release>(
            webrtc_RtpTransceiverInterface_refcounted_get(ref));
        if (!active || !transceiver) return;
        const auto receiver = own<webrtc_RtpReceiverInterface_Release>(
            webrtc_RtpReceiverInterface_refcounted_get(webrtc_RtpTransceiverInterface_receiver(transceiver.get())));
        if (!receiver) return;
        auto* refTrack = webrtc_RtpReceiverInterface_track(receiver.get());
        const auto track = own<webrtc_MediaStreamTrackInterface_Release>(
            webrtc_MediaStreamTrackInterface_refcounted_get(refTrack));
        if (!track) return;
        const auto kind = takeString(webrtc_MediaStreamTrackInterface_kind(track.get()));
        if (kind == "video" && !video) {
            video.reset(webrtc_VideoTrackInterface_refcounted_get(
                webrtc_MediaStreamTrackInterface_refcounted_cast_to_webrtc_VideoTrackInterface(refTrack)));
            if (!video) return;
            static const webrtc_VideoSinkInterface_cbs handlers{
                .OnFrame = [](const webrtc_VideoFrame* frame, void* data) { static_cast<Session*>(data)->onFrame(frame); },
                .OnDiscardedFrame = [](void*) {},
                .OnDestroy = [](void*) {},
            };
            videoSink.reset(webrtc_VideoSinkInterface_new(&handlers, this));
            const auto wants = own<webrtc_VideoSinkWants_delete>(webrtc_VideoSinkWants_new());
            webrtc_VideoTrackInterface_AddOrUpdateSink(video.get(), videoSink.get(), wants.get());
        } else if (kind == "audio" && !audio) {
            audio.reset(webrtc_AudioTrackInterface_refcounted_get(
                webrtc_MediaStreamTrackInterface_refcounted_cast_to_webrtc_AudioTrackInterface(refTrack)));
            if (!audio) return;
            static const webrtc_AudioTrackSinkInterface_cbs handlers{
                .OnData = [](const void*, int, int, std::size_t, std::size_t frames, void* data) {
                    auto& self = *static_cast<Session*>(data);
                    if (!self.active || !frames) return;
                    if (!self.playoutReceived.exchange(true) && self.callbacks.audioPlayout) self.callbacks.audioPlayout();
                },
                .OnDestroy = [](void*) {},
            };
            audioSink.reset(webrtc_AudioTrackSinkInterface_new(&handlers, this));
            webrtc_AudioTrackInterface_AddSink(audio.get(), audioSink.get());
        }
    }

    void onFrame(const webrtc_VideoFrame* frame) {
        if (!active || !frame) return;
        const auto buffer = own<webrtc_VideoFrameBuffer_Release>(
            webrtc_VideoFrameBuffer_refcounted_get(webrtc_VideoFrame_video_frame_buffer(frame)));
        if (!buffer) return;
        const int rotation = webrtc_VideoFrame_rotation(frame);
        if (callbacks.decodedFrame) {
            if (const auto* decoded = getFfmpegDecodedFrame(buffer.get())) {
                callbacks.decodedFrame(*decoded, rotation);
                return;
            }
        }
        if (!callbacks.videoFrame) return;
        auto* ref = webrtc_VideoFrameBuffer_ToI420(buffer.get());
        if (!ref) return;
        auto* planes = webrtc_I420Buffer_refcounted_get(ref);
        const auto owner = std::shared_ptr<webrtc_I420Buffer>(planes, Deleter<webrtc_I420Buffer_Release>{});
        callbacks.videoFrame({
            webrtc_I420Buffer_width(planes), webrtc_I420Buffer_height(planes),
            webrtc_I420Buffer_MutableDataY(planes), webrtc_I420Buffer_StrideY(planes),
            webrtc_I420Buffer_MutableDataU(planes), webrtc_I420Buffer_StrideU(planes),
            webrtc_I420Buffer_MutableDataV(planes), webrtc_I420Buffer_StrideV(planes),
            rotation, owner,
        });
    }

    void signalAudioReceived() {
        if (!audioReceived.exchange(true) && callbacks.audioTrack) callbacks.audioTrack();
    }

    void pollStats() {
        if (std::exchange(statsPending, true)) return;
        static webrtc_RTCStatsCollectorCallback_cbs handlers{
            .OnStatsDelivered = [](const webrtc_RTCStatsReport_refcounted* report, void* data) {
                const auto owned = own<webrtc_RTCStatsReport_Release>(webrtc_RTCStatsReport_refcounted_get_const(report));
                auto& self = *static_cast<Session*>(data);
                self.statsPending = false;
                if (!self.active) return;
                const auto text = takeString(webrtc_RTCStatsReport_ToJson(owned.get()));
                const auto reports = QJsonDocument::fromJson(QByteArray::fromStdString(text)).array();
                if (!self.audioReceived) {
                    for (const auto& item : reports) {
                        const auto entry = item.toObject();
                        if (entry.value("type") == "inbound-rtp" && entry.value("kind") == "audio" &&
                            entry.value("packetsReceived").toDouble() > 0) {
                            self.signalAudioReceived();
                            break;
                        }
                    }
                }
                if (self.callbacks.routeStats) self.callbacks.routeStats(findRoute(reports));
            },
            .OnDestroy = [](void*) {},
        };
        webrtc_PeerConnectionInterface_GetStats(peer.get(), &handlers, this);
    }

    void close() {
        active = false;
        onSignalingThread([&] {
            if (input) {
                webrtc_DataChannelInterface_UnregisterObserver(input.get());
                webrtc_DataChannelInterface_Close(input.get());
                input.reset();
                inputObserver.reset();
            }
            if (video) {
                webrtc_VideoTrackInterface_RemoveSink(video.get(), videoSink.get());
                video.reset();
                videoSink.reset();
            }
            if (audio) {
                webrtc_AudioTrackInterface_RemoveSink(audio.get(), audioSink.get());
                audio.reset();
                audioSink.reset();
            }
            if (peer) {
                webrtc_PeerConnectionInterface_Close(peer.get());
                peer.reset();
            }
            observer.reset();
            factory.reset();
        });
        signaling.reset();
        network.reset();
    }
};

RtcViewer::RtcViewer(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

RtcViewer::~RtcViewer() {
    close();
    if (initialized_) releaseRuntime();
}

bool RtcViewer::start(const std::vector<IceServer>& iceServers, bool relayOnly, ID3D11Device* device) {
    close();
    if (!callbacks_.post) {
        if (callbacks_.error) callbacks_.error("The WebRTC owner thread dispatcher is missing.");
        return false;
    }
    if (!initialized_) initialized_ = acquireRuntime();
    if (!initialized_) {
        if (callbacks_.error) callbacks_.error("Could not initialize native WebRTC.");
        return false;
    }
    auto session = std::make_shared<Session>(callbacks_);
    if (auto error = session->start(iceServers, relayOnly, device); !error.empty()) {
        if (callbacks_.error) callbacks_.error(std::move(error));
        return false;
    }
    session_ = std::move(session);
    return true;
}

void RtcViewer::acceptOffer(std::string sdp) {
    if (session_) session_->onSignalingThread([&] { session_->acceptOffer(sdp); });
}

void RtcViewer::addRemoteCandidate(IceCandidate candidate) {
    if (session_) session_->onSignalingThread([&] { session_->addCandidate(std::move(candidate)); });
}

bool RtcViewer::sendInput(std::string_view message) {
    bool sent = false;
    if (session_) session_->onSignalingThread([&] { sent = session_->sendInput(message); });
    return sent;
}

void RtcViewer::pollStats() {
    if (session_) session_->onSignalingThread([&] { session_->pollStats(); });
}

void RtcViewer::close() {
    if (const auto session = std::exchange(session_, nullptr)) session->close();
}

} // namespace evp
