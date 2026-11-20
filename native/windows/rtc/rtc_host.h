#pragma once

#include "rtc_viewer.h"

#include <cstddef>

struct webrtc_AdaptedVideoTrackSource;

namespace evp {

class HostMediaSource {
public:
    using Error = std::function<void(std::string)>;
    using VideoSize = std::function<void(int, int)>;
    using Pcm = std::function<void(const std::int16_t*, std::size_t)>;

    virtual ~HostMediaSource() = default;
    virtual bool startVideo(webrtc_AdaptedVideoTrackSource* source, int maxFps, Error error, VideoSize size) = 0;
    virtual void setVideoMaxFps(int) {}
    // Submit 10 ms of 48 kHz interleaved signed 16-bit stereo at a time.
    // The second argument of Pcm counts frames per channel.
    // After returning true, report startup once with an empty message on success, unless stopped.
    virtual bool startAudio(Pcm pcm, Error startupResult, Error runtimeError) = 0;
    // Stop and join producers before returning. The source may then be reused.
    virtual void stop() = 0;
};

class RtcHost final {
public:
    struct Callbacks {
        std::function<void(std::string)> localOffer;
        std::function<void(IceCandidate)> localCandidate;
        std::function<void(std::string)> connectionState;
        std::function<void(bool)> inputOpen;
        std::function<void(std::string)> inputMessage;
        std::function<void(RouteStats)> routeStats;
        std::function<void(std::string)> error;
        std::function<void(std::function<void()>)> post;
        std::function<void(std::string)> encoder;
        std::function<void()> audioUnavailable;
        std::function<void(std::string)> audioError;
        std::function<void(int, int)> videoSize;
    };

    // Use on the owning thread and start only after local consent. post must
    // always enqueue on the owner. Asynchronous callbacks use that dispatcher.
    explicit RtcHost(Callbacks callbacks, std::unique_ptr<HostMediaSource> media = {});
    ~RtcHost();

    RtcHost(const RtcHost&) = delete;
    RtcHost& operator=(const RtcHost&) = delete;

    bool start(const std::vector<IceServer>& iceServers, bool relayOnly, bool withAudio, int maxFps = 60);
    void acceptAnswer(std::string sdp);
    void addRemoteCandidate(IceCandidate candidate);
    bool sendInput(std::string_view message);
    bool setVideoLimits(int maxBitrateMbps, int maxFps);
    void pollStats();
    void close();

private:
    struct Session;
    Callbacks callbacks_;
    std::unique_ptr<HostMediaSource> media_;
    std::shared_ptr<Session> session_;
    bool initialized_ = false;
};

} // namespace evp
