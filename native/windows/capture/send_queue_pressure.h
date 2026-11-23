#pragma once

#include <QJsonArray>
#include <QJsonObject>

#include <atomic>
#include <chrono>

namespace evp {

// One stats writer on the signaling sequence and one reader on the encoder
// sequence. RTCOutboundRtpStreamStats measures completed packet sends, so use
// interval deltas and expire the observation if sends or polling stop.
class SendQueuePressure {
public:
    using Clock = std::chrono::steady_clock;

    void observe(const QJsonArray& reports, Clock::time_point now) {
        for (const auto& value : reports) {
            const auto report = value.toObject();
            if (report.value("type") != "outbound-rtp" || report.value("kind") != "video") continue;
            const auto id = report.value("id").toString();
            const auto packets = report.value("packetsSent").toInteger(-1);
            const auto delay = report.value("totalPacketSendDelay").toDouble(-1);
            if (id.isEmpty() || packets < 0 || delay < 0) continue;

            // Repeated cached reports are not new observations.
            if (id == id_ && packets == packets_ && delay == delay_) return;
            const bool continuous = id == id_ && packets > packets_ && delay >= delay_ &&
                now - sampled_ <= std::chrono::seconds(2);
            const bool congested = continuous && (delay - delay_) >= 0.080 * (packets - packets_);
            // Require two consecutive windows so one large IDR cannot throttle a healthy link.
            untilMs_.store(congested && lastCongested_ ? millis(now) + 1000 : 0, std::memory_order_relaxed);
            id_ = id;
            packets_ = packets;
            delay_ = delay;
            sampled_ = now;
            lastCongested_ = congested;
            return;
        }
        id_.clear();
        lastCongested_ = false;
        untilMs_.store(0, std::memory_order_relaxed);
    }

    bool active(Clock::time_point now) const {
        return millis(now) < untilMs_.load(std::memory_order_relaxed);
    }

    void recordPacingDrop() { pacingDrops_.fetch_add(1, std::memory_order_relaxed); }
    quint64 pacingDrops() const { return pacingDrops_.load(std::memory_order_relaxed); }

private:
    static qint64 millis(Clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }

    QString id_;
    qint64 packets_ = 0;
    double delay_ = 0;
    Clock::time_point sampled_{};
    bool lastCongested_ = false;
    std::atomic<qint64> untilMs_{0};
    std::atomic<quint64> pacingDrops_{0};
};

} // namespace evp
