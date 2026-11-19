#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
struct ID3D11Device;

namespace evp {

class VideoSurface;

class DecodedVideoFrame final {
public:
    int width() const;
    int height() const;
    std::uint32_t rtpTimestamp() const;
    bool isHardware() const;
    bool submit(VideoSurface& surface, int rotation = 0) const;
    bool copyI420(std::uint8_t* y, int strideY, std::uint8_t* u, int strideU,
                  std::uint8_t* v, int strideV) const;

private:
    friend class FfmpegDecoder;

    DecodedVideoFrame(std::shared_ptr<AVFrame> frame,
                      std::shared_ptr<std::atomic<std::uint64_t>> downloads);

    std::shared_ptr<AVFrame> frame_;
    std::shared_ptr<std::atomic<std::uint64_t>> downloads_;
};

class FfmpegDecoder final {
public:
    struct Stats {
        std::uint64_t gpuFrames = 0;
        std::uint64_t cpuFrames = 0;
        std::uint64_t hardwareDownloads = 0;
        std::uint64_t submitted = 0;
    };

    explicit FfmpegDecoder(VideoSurface& surface);
    FfmpegDecoder(ID3D11Device* device, std::function<bool(const DecodedVideoFrame&)> onFrame);
    ~FfmpegDecoder();

    // Open on the surface's Qt thread to acquire its presentation device. After
    // open, use decode/flush from one caller thread and keep the surface alive.
    bool open(const AVCodecParameters& parameters, bool preferHardware = true);
    bool decode(const AVPacket& packet);
    bool openH264(bool preferHardware = true);
    bool decodeAnnexB(std::span<const std::uint8_t> bytes, std::uint32_t rtpTimestamp);
    bool flush();
    bool hasHardwareDevice() const;
    Stats stats() const;
    std::string lastError() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace evp
