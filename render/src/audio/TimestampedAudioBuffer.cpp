#include "TimestampedAudioBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace RENDER {

TimestampedAudioBuffer::TimestampedAudioBuffer(
    int sample_rate,
    int channels,
    std::uint32_t max_buffer_duration_ms,
    std::uint32_t max_gap_fill_ms,
    std::uint32_t discontinuity_threshold_ms)
    : m_sample_rate(sample_rate),
      m_channels(channels),
      m_capacity_frames(
          sample_rate > 0 && channels > 0
              ? static_cast<std::size_t>(
                    static_cast<std::uint64_t>(sample_rate) *
                    max_buffer_duration_ms / 1000U)
              : 0),
      m_max_gap_fill_frames(
          sample_rate > 0
              ? static_cast<std::size_t>(
                    static_cast<std::uint64_t>(sample_rate) *
                    max_gap_fill_ms / 1000U)
              : 0),
      m_discontinuity_threshold_frames(
          sample_rate > 0
              ? static_cast<std::size_t>(
                    static_cast<std::uint64_t>(sample_rate) *
                    discontinuity_threshold_ms / 1000U)
              : 0),
      m_buffer(
          m_capacity_frames *
              static_cast<std::size_t>(
                  channels > 0 ? channels : 0),
          0.0F)
{
}

TimestampedAudioPushResult
TimestampedAudioBuffer::Push(
    const AudioFrame& frame)
{
    if (!IsConfigured() ||
        !frame.IsValid() ||
        frame.sample_rate != m_sample_rate ||
        frame.channels != m_channels ||
        frame.timestamp_us <= 0) {
        return TimestampedAudioPushResult::kInvalid;
    }

    const auto* input_data =
        reinterpret_cast<const float*>(
            frame.data.get());

    if (!input_data) {
        return TimestampedAudioPushResult::kInvalid;
    }

    std::size_t input_frames =
        static_cast<std::size_t>(frame.samples);

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_timeline_initialized) {
        ResetTimelineLocked(frame.timestamp_us);
        WriteFramesLocked(input_data, input_frames);

        return TimestampedAudioPushResult::kAccepted;
    }

    /*
     * 当前缓冲区逻辑末尾，即下一帧应当对应的媒体时间。
     */
    const std::int64_t expected_timestamp_us =
        TimestampForFrameOffsetLocked(
            m_write_frame_offset);

    const std::int64_t timestamp_difference_us =
        frame.timestamp_us -
        expected_timestamp_us;

    const std::int64_t frame_difference =
        MicrosecondsToFrames(
            timestamp_difference_us);

    const std::int64_t discontinuity_frames =
        static_cast<std::int64_t>(
            m_discontinuity_threshold_frames);

    /*
     * 时间戳发生大幅向前或向后跳变时，旧时间线已经不能继续使用。
     */
    if (frame_difference > discontinuity_frames ||
        frame_difference < -discontinuity_frames) {
        ResetTimelineLocked(frame.timestamp_us);
        WriteFramesLocked(input_data, input_frames);

        return TimestampedAudioPushResult::
            kAcceptedAfterReset;
    }

    TimestampedAudioPushResult result =
        TimestampedAudioPushResult::kAccepted;

    if (frame_difference > 0) {
        const auto gap_frames =
            static_cast<std::size_t>(
                frame_difference);

        /*
         * 小间隙使用静音补齐。
         *
         * 中等间隙虽然尚未超过时间线跳变阈值，但继续填充会明显
         * 增加播放延迟，因此也直接重建缓冲时间线。
         */
        if (gap_frames >
            m_max_gap_fill_frames) {
            ResetTimelineLocked(
                frame.timestamp_us);

            WriteFramesLocked(
                input_data,
                input_frames);

            return TimestampedAudioPushResult::
                kAcceptedAfterReset;
        }

        WriteSilenceLocked(gap_frames);

        result =
            TimestampedAudioPushResult::
                kAcceptedWithSilence;
    } else if (frame_difference < 0) {
        /*
         * 当前输入与已经缓存的数据存在重叠。
         * 跳过输入开头已经存在的采样帧，只追加新的尾部。
         */
        const auto overlap_frames =
            static_cast<std::size_t>(
                -frame_difference);

        if (overlap_frames >= input_frames) {
            return TimestampedAudioPushResult::
                kDroppedLate;
        }

        const std::size_t skipped_samples =
            overlap_frames *
            static_cast<std::size_t>(
                m_channels);

        input_data += skipped_samples;
        input_frames -= overlap_frames;
    }

    WriteFramesLocked(
        input_data,
        input_frames);

    return result;
}

TimestampedAudioReadResult
TimestampedAudioBuffer::ReadAt(
    std::int64_t start_timestamp_us,
    float* output,
    std::uint32_t frame_count)
{
    TimestampedAudioReadResult result;
    result.requested_frames = frame_count;
    result.start_timestamp_us =
        start_timestamp_us;

    if (!output ||
        frame_count == 0 ||
        !IsConfigured() ||
        start_timestamp_us <= 0) {
        return result;
    }

    const std::size_t requested_frames =
        static_cast<std::size_t>(
            frame_count);

    const std::size_t requested_samples =
        requested_frames *
        static_cast<std::size_t>(
            m_channels);

    /*
     * 无论缓冲区是否有数据，调用成功后输出区都必须完全初始化。
     */
    std::memset(
        output,
        0,
        requested_samples * sizeof(float));

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_timeline_initialized ||
        m_available_frames == 0) {
        result.trailing_silence_frames =
            frame_count;

        return result;
    }

    std::int64_t first_timestamp_us =
        TimestampForFrameOffsetLocked(
            m_read_frame_offset);

    /*
     * 请求时间晚于缓存队首时，队首之前的数据已经过期。
     */
    if (start_timestamp_us >
        first_timestamp_us) {
        const std::int64_t discard_difference_us =
            start_timestamp_us -
            first_timestamp_us;

        const std::int64_t discard_frame_count =
            MicrosecondsToFrames(
                discard_difference_us);

        if (discard_frame_count > 0) {
            DiscardFramesLocked(
                std::min(
                    static_cast<std::size_t>(
                        discard_frame_count),
                    m_available_frames));
        }

        if (m_available_frames == 0) {
            result.trailing_silence_frames =
                frame_count;

            return result;
        }

        first_timestamp_us =
            TimestampForFrameOffsetLocked(
                m_read_frame_offset);
    }

    std::size_t leading_silence_frames = 0;

    /*
     * 请求时间早于缓存队首时，输出开头缺少音频，使用静音补齐。
     */
    if (start_timestamp_us <
        first_timestamp_us) {
        const std::int64_t missing_duration_us =
            first_timestamp_us -
            start_timestamp_us;

        const std::int64_t missing_frames =
            MicrosecondsToFrames(
                missing_duration_us);

        if (missing_frames > 0) {
            leading_silence_frames =
                std::min(
                    static_cast<std::size_t>(
                        missing_frames),
                    requested_frames);
        }
    }

    result.leading_silence_frames =
        static_cast<std::uint32_t>(
            leading_silence_frames);

    if (leading_silence_frames >=
        requested_frames) {
        return result;
    }

    const std::size_t remaining_frames =
        requested_frames -
        leading_silence_frames;

    const std::size_t readable_frames =
        std::min(
            remaining_frames,
            m_available_frames);

    float* audio_output =
        output +
        leading_silence_frames *
            static_cast<std::size_t>(
                m_channels);

    const std::size_t copied_frames =
        ReadFramesLocked(
            audio_output,
            readable_frames);

    result.copied_frames =
        static_cast<std::uint32_t>(
            copied_frames);

    result.trailing_silence_frames =
        static_cast<std::uint32_t>(
            requested_frames -
            leading_silence_frames -
            copied_frames);

    return result;
}

std::optional<std::int64_t>
TimestampedAudioBuffer::GetFirstTimestampUs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_timeline_initialized ||
        m_available_frames == 0) {
        return std::nullopt;
    }

    return TimestampForFrameOffsetLocked(
        m_read_frame_offset);
}

std::optional<std::int64_t>
TimestampedAudioBuffer::GetEndTimestampUs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_timeline_initialized ||
        m_available_frames == 0) {
        return std::nullopt;
    }

    return TimestampForFrameOffsetLocked(
        m_write_frame_offset);
}

std::size_t
TimestampedAudioBuffer::GetBufferedFrameCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_available_frames;
}

void TimestampedAudioBuffer::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_read_position_frames = 0;
    m_write_position_frames = 0;
    m_available_frames = 0;

    m_timeline_initialized = false;
    m_timeline_anchor_us = 0;

    m_read_frame_offset = 0;
    m_write_frame_offset = 0;
}

void TimestampedAudioBuffer::ResetTimelineLocked(
    std::int64_t timestamp_us) noexcept
{
    m_read_position_frames = 0;
    m_write_position_frames = 0;
    m_available_frames = 0;

    m_timeline_initialized = true;
    m_timeline_anchor_us = timestamp_us;

    m_read_frame_offset = 0;
    m_write_frame_offset = 0;
}

void TimestampedAudioBuffer::WriteFramesLocked(
    const float* data,
    std::size_t frame_count)
{
    if (!data ||
        frame_count == 0 ||
        m_capacity_frames == 0) {
        return;
    }

    const std::size_t channel_count =
        static_cast<std::size_t>(
            m_channels);

    /*
     * 单次输入超过整个缓冲容量时，只保留最新部分。
     *
     * 逻辑写偏移仍然推进完整frame_count，从而保证保留下来的
     * 数据仍具有正确时间戳。
     */
    if (frame_count >= m_capacity_frames) {
        const std::size_t retained_frames =
            m_capacity_frames;

        const std::size_t skipped_frames =
            frame_count -
            retained_frames;

        const float* retained_data =
            data +
            skipped_frames *
                channel_count;

        std::memcpy(
            m_buffer.data(),
            retained_data,
            retained_frames *
                channel_count *
                sizeof(float));

        m_write_frame_offset +=
            frame_count;

        m_read_frame_offset =
            m_write_frame_offset -
            retained_frames;

        m_read_position_frames = 0;
        m_write_position_frames = 0;
        m_available_frames =
            retained_frames;

        return;
    }

    MakeRoomLocked(frame_count);

    const std::size_t first_copy_frames =
        std::min(
            frame_count,
            m_capacity_frames -
                m_write_position_frames);

    std::memcpy(
        m_buffer.data() +
            m_write_position_frames *
                channel_count,
        data,
        first_copy_frames *
            channel_count *
            sizeof(float));

    const std::size_t second_copy_frames =
        frame_count -
        first_copy_frames;

    if (second_copy_frames > 0) {
        std::memcpy(
            m_buffer.data(),
            data +
                first_copy_frames *
                    channel_count,
            second_copy_frames *
                channel_count *
                sizeof(float));
    }

    m_write_position_frames =
        (m_write_position_frames +
         frame_count) %
        m_capacity_frames;

    m_available_frames += frame_count;
    m_write_frame_offset += frame_count;
}

void TimestampedAudioBuffer::WriteSilenceLocked(
    std::size_t frame_count)
{
    if (frame_count == 0 ||
        m_capacity_frames == 0) {
        return;
    }

    const std::size_t channel_count =
        static_cast<std::size_t>(
            m_channels);

    if (frame_count >= m_capacity_frames) {
        std::fill(
            m_buffer.begin(),
            m_buffer.end(),
            0.0F);

        m_write_frame_offset +=
            frame_count;

        m_read_frame_offset =
            m_write_frame_offset -
            m_capacity_frames;

        m_read_position_frames = 0;
        m_write_position_frames = 0;
        m_available_frames =
            m_capacity_frames;

        return;
    }

    MakeRoomLocked(frame_count);

    const std::size_t first_fill_frames =
        std::min(
            frame_count,
            m_capacity_frames -
                m_write_position_frames);

    std::fill_n(
        m_buffer.data() +
            m_write_position_frames *
                channel_count,
        first_fill_frames *
            channel_count,
        0.0F);

    const std::size_t second_fill_frames =
        frame_count -
        first_fill_frames;

    if (second_fill_frames > 0) {
        std::fill_n(
            m_buffer.data(),
            second_fill_frames *
                channel_count,
            0.0F);
    }

    m_write_position_frames =
        (m_write_position_frames +
         frame_count) %
        m_capacity_frames;

    m_available_frames += frame_count;
    m_write_frame_offset += frame_count;
}

std::size_t TimestampedAudioBuffer::ReadFramesLocked(
    float* output,
    std::size_t frame_count)
{
    if (!output ||
        frame_count == 0 ||
        m_available_frames == 0) {
        return 0;
    }

    const std::size_t read_frames =
        std::min(
            frame_count,
            m_available_frames);

    const std::size_t channel_count =
        static_cast<std::size_t>(
            m_channels);

    const std::size_t first_copy_frames =
        std::min(
            read_frames,
            m_capacity_frames -
                m_read_position_frames);

    std::memcpy(
        output,
        m_buffer.data() +
            m_read_position_frames *
                channel_count,
        first_copy_frames *
            channel_count *
            sizeof(float));

    const std::size_t second_copy_frames =
        read_frames -
        first_copy_frames;

    if (second_copy_frames > 0) {
        std::memcpy(
            output +
                first_copy_frames *
                    channel_count,
            m_buffer.data(),
            second_copy_frames *
                channel_count *
                sizeof(float));
    }

    DiscardFramesLocked(read_frames);
    return read_frames;
}

void TimestampedAudioBuffer::DiscardFramesLocked(
    std::size_t frame_count) noexcept
{
    if (frame_count == 0 ||
        m_available_frames == 0 ||
        m_capacity_frames == 0) {
        return;
    }

    const std::size_t discard_frames =
        std::min(
            frame_count,
            m_available_frames);

    m_read_position_frames =
        (m_read_position_frames +
         discard_frames) %
        m_capacity_frames;

    m_available_frames -=
        discard_frames;

    m_read_frame_offset +=
        discard_frames;

    if (m_available_frames == 0) {
        m_read_position_frames =
            m_write_position_frames;

        m_read_frame_offset =
            m_write_frame_offset;
    }
}

void TimestampedAudioBuffer::MakeRoomLocked(
    std::size_t required_frames) noexcept
{
    if (required_frames == 0 ||
        m_capacity_frames == 0) {
        return;
    }

    if (required_frames >=
        m_capacity_frames) {
        DiscardFramesLocked(
            m_available_frames);

        return;
    }

    const std::size_t free_frames =
        m_capacity_frames -
        m_available_frames;

    if (required_frames >
        free_frames) {
        DiscardFramesLocked(
            required_frames -
            free_frames);
    }
}

std::int64_t
TimestampedAudioBuffer::MicrosecondsToFrames(
    std::int64_t duration_us) const noexcept
{
    if (m_sample_rate <= 0 ||
        duration_us == 0) {
        return 0;
    }

    const long double frame_count =
        static_cast<long double>(
            duration_us) *
        static_cast<long double>(
            m_sample_rate) /
        1'000'000.0L;

    if (frame_count >=
        static_cast<long double>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<
            std::int64_t>::max();
    }

    if (frame_count <=
        static_cast<long double>(
            std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<
            std::int64_t>::min();
    }

    return static_cast<std::int64_t>(
        std::llround(frame_count));
}

std::int64_t
TimestampedAudioBuffer::FramesToMicroseconds(
    std::int64_t frame_count) const noexcept
{
    if (m_sample_rate <= 0 ||
        frame_count == 0) {
        return 0;
    }

    const long double duration_us =
        static_cast<long double>(
            frame_count) *
        1'000'000.0L /
        static_cast<long double>(
            m_sample_rate);

    if (duration_us >=
        static_cast<long double>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<
            std::int64_t>::max();
    }

    if (duration_us <=
        static_cast<long double>(
            std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<
            std::int64_t>::min();
    }

    return static_cast<std::int64_t>(
        std::llround(duration_us));
}

std::int64_t
TimestampedAudioBuffer::
TimestampForFrameOffsetLocked(
    std::uint64_t frame_offset) const noexcept
{
    if (frame_offset >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<
            std::int64_t>::max();
    }

    const std::int64_t offset_us =
        FramesToMicroseconds(
            static_cast<std::int64_t>(
                frame_offset));

    if (offset_us > 0 &&
        m_timeline_anchor_us >
            std::numeric_limits<std::int64_t>::max() -
                offset_us) {
        return std::numeric_limits<
            std::int64_t>::max();
    }

    return m_timeline_anchor_us +
           offset_us;
}

} // namespace RENDER