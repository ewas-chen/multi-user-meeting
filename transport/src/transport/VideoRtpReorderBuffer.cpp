#include "VideoRtpReorderBuffer.h"

#include <rtc/rtp.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace TRANSPORT {

VideoRtpReorderBuffer::VideoRtpReorderBuffer(
    std::chrono::milliseconds max_wait,
    std::size_t max_buffered_frames,
    LossCallback loss_callback)
    : m_max_wait(std::max(max_wait, std::chrono::milliseconds(1))),
      m_max_buffered_frames(std::max<std::size_t>(max_buffered_frames, 2)),
      m_loss_callback(std::move(loss_callback)) {}

void VideoRtpReorderBuffer::incoming(
    rtc::message_vector& messages,
    const rtc::message_callback& send) {
    (void)send;

    rtc::message_vector output;
    output.reserve(messages.size());

    const Clock::time_point now = Clock::now();
    bool frame_lost = false;
    bool notify_loss = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& message : messages) {
            if (!message || message->type != rtc::Message::Binary) {
                output.push_back(std::move(message));
                continue;
            }

            ++m_statistics.received_packets;

            if (message->size() < sizeof(rtc::RtpHeader)) {
                ++m_statistics.invalid_packets;
                continue;
            }

            const auto* header =
                reinterpret_cast<const rtc::RtpHeader*>(message->data());

            if (header->version() != 2) {
                ++m_statistics.invalid_packets;
                continue;
            }

            const std::size_t header_size = header->getSize();
            if (message->size() < header_size) {
                ++m_statistics.invalid_packets;
                continue;
            }

            const std::size_t total_header_size =
                header_size + header->getExtensionHeaderSize();

            if (message->size() < total_header_size) {
                ++m_statistics.invalid_packets;
                continue;
            }

            const std::uint16_t rtp_sequence = header->seqNumber();
            const std::uint32_t rtp_timestamp = header->timestamp();
            const bool marker = header->marker();

            const std::int64_t sequence =
                UnwrapSequence(rtp_sequence);

            if (m_next_sequence && sequence < *m_next_sequence) {
                ++m_statistics.late_packets;
                continue;
            }

            auto [frame_it, inserted] = m_frames.try_emplace(
                rtp_timestamp);

            BufferedFrame& frame = frame_it->second;

            if (inserted) {
                frame.rtp_timestamp = rtp_timestamp;
                frame.min_sequence = sequence;
                frame.marker_sequence = sequence;
                frame.first_arrival = now;
            } else {
                frame.min_sequence =
                    std::min(frame.min_sequence, sequence);
            }

            const auto packet_result =
                frame.packets.emplace(sequence, std::move(message));

            if (!packet_result.second) {
                ++m_statistics.duplicate_packets;
                continue;
            }

            if (sequence < m_highest_sequence) {
                ++m_statistics.reordered_packets;
            }

            if (marker) {
                frame.marker_received = true;
                frame.marker_sequence = sequence;
            }
        }

        ReleaseReadyFrames(now, output, frame_lost);

        if (frame_lost &&
            (m_last_loss_notification == Clock::time_point{} ||
             now - m_last_loss_notification >= m_loss_notify_interval)) {
            m_last_loss_notification = now;
            notify_loss = true;
        }
    }

    messages.swap(output);

    if (notify_loss && m_loss_callback) {
        try {
            m_loss_callback();
        } catch (...) {
            // Do not allow exceptions to escape into libdatachannel.
        }
    }
}

std::int64_t VideoRtpReorderBuffer::UnwrapSequence(
    std::uint16_t sequence) {
    if (!m_sequence_initialized) {
        m_sequence_initialized = true;
        m_highest_sequence = sequence;
        return m_highest_sequence;
    }

    const std::int64_t cycle =
        m_highest_sequence & ~static_cast<std::int64_t>(0xFFFF);

    std::int64_t candidate = cycle + sequence;

    if (candidate + 32768 < m_highest_sequence) {
        candidate += 65536;
    } else if (candidate - 32768 > m_highest_sequence) {
        candidate -= 65536;
    }

    if (candidate > m_highest_sequence) {
        m_highest_sequence = candidate;
    }

    return candidate;
}

void VideoRtpReorderBuffer::ReleaseReadyFrames(
    Clock::time_point now,
    rtc::message_vector& output,
    bool& frame_lost) {
    while (!m_frames.empty()) {
        FrameIterator oldest = FindOldestFrame();
        BufferedFrame& frame = oldest->second;

        const bool force_process =
            m_frames.size() > m_max_buffered_frames;

        const bool wait_expired =
            now - frame.first_arrival >= m_max_wait;

        const std::optional<std::int64_t> next_frame_sequence =
            FindNextFrameSequence(frame);

        if (!frame.marker_received) {
            if (!force_process &&
                (!next_frame_sequence || !wait_expired)) {
                break;
            }

            ++m_statistics.dropped_frames;
            // A missing marker means at least one packet was lost.
            ++m_statistics.missing_packets;
            frame_lost = true;

            if (next_frame_sequence) {
                m_next_sequence = *next_frame_sequence;
            } else {
                m_next_sequence = frame.min_sequence +
                                  static_cast<std::int64_t>(
                                      frame.packets.size());
            }

            m_frames.erase(oldest);
            continue;
        }

        std::int64_t first_sequence = frame.min_sequence;

        if (m_next_sequence) {
            first_sequence = *m_next_sequence;
        } else if (!force_process && !wait_expired) {
            // The first frame has no previous marker boundary. Wait briefly
            // so packets with earlier sequence numbers can still arrive.
            break;
        }

        if (frame.marker_sequence < first_sequence) {
            m_statistics.late_packets += frame.packets.size();
            m_frames.erase(oldest);
            continue;
        }

        const std::uint64_t packet_span =
            static_cast<std::uint64_t>(
                frame.marker_sequence - first_sequence) + 1;

        bool complete = packet_span <= 4096;

        if (complete) {
            for (std::int64_t sequence = first_sequence;
                 sequence <= frame.marker_sequence;
                 ++sequence) {
                if (frame.packets.find(sequence) ==
                    frame.packets.end()) {
                    complete = false;
                    break;
                }
            }
        }

        if (!complete && !force_process && !wait_expired) {
            break;
        }

        if (!complete) {
            ++m_statistics.dropped_frames;
            frame_lost = true;

            const std::uint64_t present =
                static_cast<std::uint64_t>(frame.packets.size());

            m_statistics.missing_packets +=
                packet_span > present ? packet_span - present : 1;

            m_next_sequence = frame.marker_sequence + 1;
            m_frames.erase(oldest);
            continue;
        }

        for (std::int64_t sequence = first_sequence;
             sequence <= frame.marker_sequence;
             ++sequence) {
            auto packet = frame.packets.find(sequence);
            output.push_back(std::move(packet->second));
            ++m_statistics.released_packets;
        }

        ++m_statistics.released_frames;
        m_next_sequence = frame.marker_sequence + 1;
        m_frames.erase(oldest);
    }
}

VideoRtpReorderBuffer::FrameIterator
VideoRtpReorderBuffer::FindOldestFrame() {
    return std::min_element(
        m_frames.begin(), m_frames.end(),
        [](const auto& left, const auto& right) {
            return left.second.min_sequence <
                   right.second.min_sequence;
        });
}

std::optional<std::int64_t>
VideoRtpReorderBuffer::FindNextFrameSequence(
    const BufferedFrame& current) const {
    std::optional<std::int64_t> result;

    for (const auto& [timestamp, frame] : m_frames) {
        (void)timestamp;

        if (frame.rtp_timestamp == current.rtp_timestamp ||
            frame.min_sequence <= current.min_sequence) {
            continue;
        }

        if (!result || frame.min_sequence < *result) {
            result = frame.min_sequence;
        }
    }

    return result;
}

VideoRtpReorderBuffer::Statistics
VideoRtpReorderBuffer::GetStatistics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_statistics;
}

void VideoRtpReorderBuffer::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_frames.clear();
    m_next_sequence.reset();
    m_sequence_initialized = false;
    m_highest_sequence = 0;
    m_last_loss_notification = {};
    m_statistics = {};
}

} // namespace TRANSPORT