#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11Device;

namespace evp {

class DecodedVideoFrame;
struct CursorPacket;

struct IceServer {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

struct IceCandidate {
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex = 0;
};

struct I420Frame {
    int width;
    int height;
    const std::uint8_t* y;
    int strideY;
    const std::uint8_t* u;
    int strideU;
    const std::uint8_t* v;
    int strideV;
    int rotation;
    std::shared_ptr<const void> owner;
};

struct RouteStats {
    // An empty kind means that ICE has not selected a usable pair.
    std::string kind;
    std::string local;
    std::string remote;
};

class RtcViewer final {
public:
    struct Callbacks {
        std::function<void(std::string)> localAnswer;
        std::function<void(IceCandidate)> localCandidate;
        std::function<void(std::string)> connectionState;
        std::function<void(bool)> inputOpen;
        std::function<void(std::string)> inputMessage;
        std::function<void(const CursorPacket&)> cursorPacket;
        std::function<void()> cursorClosed;
        std::function<void(const I420Frame&)> videoFrame;
        std::function<void(RouteStats)> routeStats;
        std::function<void(std::string)> error;
        // audioTrack reports received RTP. audioPlayout reports decoded PCM
        // from the default audio device's playout path.
        std::function<void()> audioTrack;
        std::function<void()> audioPlayout;
        // Copy the owned frame value if the callback queues presentation.
        std::function<void(const DecodedVideoFrame&, int rotation)> decodedFrame;
        std::function<void(std::function<void()>)> post;
    };

    // Call methods from the owning thread. Callbacks may run on WebRTC threads
    // and must return without calling back into RtcViewer or waiting on its owner.
    // post must enqueue work on the owning thread, even when already on it.
    explicit RtcViewer(Callbacks callbacks);
    ~RtcViewer();

    RtcViewer(const RtcViewer&) = delete;
    RtcViewer& operator=(const RtcViewer&) = delete;

    bool start(const std::vector<IceServer>& iceServers, bool relayOnly = false, ID3D11Device* device = nullptr);
    void acceptOffer(std::string sdp);
    void addRemoteCandidate(IceCandidate candidate);
    bool sendInput(std::string_view message);
    bool acceptSeparateCursor(std::uint64_t epoch);
    void closeSeparateCursor();
    void pollStats();
    void close();

private:
    struct Session;
    Callbacks callbacks_;
    std::shared_ptr<Session> session_;
    bool initialized_ = false;
};

} // namespace evp
