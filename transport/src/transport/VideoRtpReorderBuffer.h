#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <rtc/mediahandler.hpp>
#include <rtc/message.hpp>

namespace TRANSPORT {

/**
 * @brief 视频RTP短时重排序缓冲
 *
 * 该处理器位于RtcpReceivingSession与H264RtpDepacketizer之间。
 * 它按RTP时间戳保存一帧的全部分片，确认序列号连续后才把
 * 完整帧交给H264解包器，避免Marker包提前到达时过早组帧。
 *
 * 该类只能恢复乱序，不能恢复真正丢失的RTP包。等待超时后会
 * 丢弃整帧并限频通知上层请求关键帧，后续可在此基础上增加NACK。
 */
class VideoRtpReorderBuffer final : public rtc::MediaHandler {
public:
    using LossCallback = std::function<void()>;

    struct Statistics {
        std::uint64_t received_packets{0};
        std::uint64_t released_packets{0};
        std::uint64_t reordered_packets{0};
        std::uint64_t duplicate_packets{0};
        std::uint64_t late_packets{0};
        std::uint64_t invalid_packets{0};
        std::uint64_t released_frames{0};
        std::uint64_t dropped_frames{0};
        std::uint64_t missing_packets{0};
    };

    explicit VideoRtpReorderBuffer(
        std::chrono::milliseconds max_wait = std::chrono::milliseconds(50),
        std::size_t max_buffered_frames = 6,
        LossCallback loss_callback = {});

    void incoming(rtc::message_vector& messages,
                  const rtc::message_callback& send) override;

    Statistics GetStatistics() const;
    void Reset();

private:
    using Clock = std::chrono::steady_clock;

    struct BufferedFrame {
        std::uint32_t rtp_timestamp{0};
        std::int64_t min_sequence{0};
        std::int64_t marker_sequence{0};
        bool marker_received{false};
        Clock::time_point first_arrival{};
        std::map<std::int64_t, rtc::message_ptr> packets;
    };

    std::int64_t UnwrapSequence(std::uint16_t sequence);
    void ReleaseReadyFrames(Clock::time_point now,
                            rtc::message_vector& output,
                            bool& frame_lost);

    using FrameIterator =
        std::unordered_map<std::uint32_t, BufferedFrame>::iterator;

    FrameIterator FindOldestFrame();
    std::optional<std::int64_t>
    FindNextFrameSequence(const BufferedFrame& current) const;

private:
    const std::chrono::milliseconds m_max_wait;
    const std::size_t m_max_buffered_frames;
    const std::chrono::milliseconds m_loss_notify_interval{500};
    LossCallback m_loss_callback;

    mutable std::mutex m_mutex;
    std::unordered_map<std::uint32_t, BufferedFrame> m_frames;
    std::optional<std::int64_t> m_next_sequence;

    bool m_sequence_initialized{false};
    std::int64_t m_highest_sequence{0};
    Clock::time_point m_last_loss_notification{};
    Statistics m_statistics;
};

} // namespace TRANSPORT