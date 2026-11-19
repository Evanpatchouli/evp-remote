#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

namespace evp {

class LoopbackAudio {
public:
    using AudioCallback = std::function<void(const std::int16_t*, std::size_t)>;
    using ErrorCallback = std::function<void(QString)>;

    ~LoopbackAudio();

    // Start only after local consent. Samples are interleaved 48 kHz signed 16-bit stereo.
    // The count is frames per channel. Data is borrowed through the callback.
    // Callbacks run on the loopback thread. Schedule teardown on the owner thread.
    bool start(AudioCallback onAudio, ErrorCallback onError, ErrorCallback onRuntimeError = {});
    void stop();

private:
    std::jthread worker_;
};

}  // namespace evp
