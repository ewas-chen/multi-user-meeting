#pragma once

#include "RenderDefine.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace RENDER {

/**
 * @brief 音频写入结果
 */
enum class TimestampedAudioPushResult : std::uint8_t
{
    // 音频连续写入缓冲区
    kAccepted = 0,

    // 音频之间存在较小间隙，已自动插入静音
    kAcceptedWithSilence,

    // 检测到时间线跳变，清空旧数据后写入新音频
    kAcceptedAfterReset,

    // 整个输入块都是已经处理过的旧数据
    kDroppedLate,

    // 输入音频格式、时间戳或数据无效
    kInvalid
};

/**
 * @brief 按时间读取音频的结果
 */
struct TimestampedAudioReadResult
{
    // 本次请求的每声道采样帧数
    std::uint32_t requested_frames{0};

    // 实际从缓冲区复制的有效音频帧数
    std::uint32_t copied_frames{0};

    // 输出开始部分补入的静音帧数
    std::uint32_t leading_silence_frames{0};

    // 输出末尾补入的静音帧数
    std::uint32_t trailing_silence_frames{0};

    // 本次输出第一帧对应的公共媒体时间
    std::int64_t start_timestamp_us{0};

    bool HasAudio() const noexcept
    {
        return copied_frames > 0;
    }

    bool HasUnderrun() const noexcept
    {
        return copied_frames < requested_frames;
    }
};

/**
 * @brief 单个用户的带时间戳音频环形缓冲区
 *
 * 保存交错排列的Float32 PCM，同时保留每个采样帧在公共媒体
 * 时间线中的位置。
 *
 * AudioMixer可以选择统一的混音时间点，然后对每个用户调用
 * ReadAt()。如果某个用户在该时间段没有音频，本类会自动输出静音，
 * 从而保证不同用户的声音按照时间戳对齐，而不是按照到达顺序混合。
 *
 * 数据结构使用固定容量环形缓冲区，初始化后不会因Push或Read
 * 频繁扩容、insert或erase。
 */
class RENDER_ENGINE_LOCAL TimestampedAudioBuffer final
{
public:
    /**
     * @param sample_rate 音频采样率，例如48000
     * @param channels 声道数
     * @param max_buffer_duration_ms 最大缓存时长
     * @param max_gap_fill_ms 可使用静音补齐的最大音频间隙
     * @param discontinuity_threshold_ms 时间线跳变判断阈值
     */
    TimestampedAudioBuffer(
        int sample_rate,
        int channels,
        std::uint32_t max_buffer_duration_ms = 500,
        std::uint32_t max_gap_fill_ms = 100,
        std::uint32_t discontinuity_threshold_ms = 500);

    ~TimestampedAudioBuffer() = default;

    TimestampedAudioBuffer(const TimestampedAudioBuffer&) = delete;
    TimestampedAudioBuffer& operator=(const TimestampedAudioBuffer&) = delete;
    TimestampedAudioBuffer(TimestampedAudioBuffer&&) = delete;
    TimestampedAudioBuffer& operator=(TimestampedAudioBuffer&&) = delete;

    /**
     * @brief 写入一块带时间戳的Float32 PCM
     *
     * frame.timestamp_us表示该音频块第一个采样帧的公共媒体时间。
     *
     * 输入与已有数据之间出现：
     *
     * - 小间隙：插入静音；
     * - 少量重叠：跳过重复的输入采样；
     * - 大幅跳变：清空旧数据并重建时间线；
     * - 完全过期：丢弃当前输入块。
     */
    TimestampedAudioPushResult Push(
        const AudioFrame& frame);

    /**
     * @brief 从指定媒体时间开始读取音频
     *
     * @param start_timestamp_us 输出第一帧对应的公共媒体时间
     * @param output 输出Float32 PCM缓冲区
     * @param frame_count 请求的每声道采样帧数
     *
     * output必须能够保存：
     *
     *     frame_count * channels
     *
     * 个float。
     *
     * 输出缓冲区会被完整写入。没有音频覆盖的区域使用静音补齐。
     * 已经早于start_timestamp_us的缓存数据会被丢弃。
     */
    TimestampedAudioReadResult ReadAt(
        std::int64_t start_timestamp_us,
        float* output,
        std::uint32_t frame_count);

    /**
     * @brief 获取当前最早可读音频帧的时间戳
     */
    std::optional<std::int64_t>
    GetFirstTimestampUs() const;

    /**
     * @brief 获取当前已缓存音频末尾的时间戳
     *
     * 返回的是下一帧待写入位置对应的时间，即半开区间的结束位置。
     */
    std::optional<std::int64_t>
    GetEndTimestampUs() const;

    /**
     * @brief 当前可读取的每声道采样帧数
     */
    std::size_t GetBufferedFrameCount() const;

    /**
     * @brief 清空音频并重置时间线
     *
     * 不释放环形缓冲区已经分配的内存。
     */
    void Reset();

    bool IsConfigured() const noexcept
    {
        return m_sample_rate > 0 &&
               m_channels > 0 &&
               m_capacity_frames > 0;
    }

    int GetSampleRate() const noexcept
    {
        return m_sample_rate;
    }

    int GetChannels() const noexcept
    {
        return m_channels;
    }

private:
    /**
     * @brief 清空缓冲并以新音频时间戳建立时间线
     *
     * 调用时必须持有m_mutex。
     */
    void ResetTimelineLocked(
        std::int64_t timestamp_us) noexcept;

    /**
     * @brief 向环形缓冲区写入音频采样
     *
     * frame_count是每声道采样帧数，data按声道交错排列。
     * 调用时必须持有m_mutex。
     */
    void WriteFramesLocked(
        const float* data,
        std::size_t frame_count);

    /**
     * @brief 向环形缓冲区写入静音
     *
     * 调用时必须持有m_mutex。
     */
    void WriteSilenceLocked(
        std::size_t frame_count);

    /**
     * @brief 从环形缓冲区读取并消费音频
     *
     * 调用时必须持有m_mutex。
     */
    std::size_t ReadFramesLocked(
        float* output,
        std::size_t frame_count);

    /**
     * @brief 丢弃最早的音频帧
     *
     * 调用时必须持有m_mutex。
     */
    void DiscardFramesLocked(
        std::size_t frame_count) noexcept;

    /**
     * @brief 确保有足够空间写入新音频
     *
     * 空间不足时丢弃最早的数据，实时播放优先保证低延迟。
     * 调用时必须持有m_mutex。
     */
    void MakeRoomLocked(
        std::size_t required_frames) noexcept;

    /**
     * @brief 将相对时间转换为采样帧偏移
     *
     * 使用最接近的采样位置，降低微秒整数时间产生的舍入误差。
     */
    std::int64_t MicrosecondsToFrames(
        std::int64_t duration_us) const noexcept;

    /**
     * @brief 将采样帧偏移转换为微秒
     */
    std::int64_t FramesToMicroseconds(
        std::int64_t frame_count) const noexcept;

    /**
     * @brief 根据时间线锚点计算指定帧位置的时间戳
     */
    std::int64_t TimestampForFrameOffsetLocked(
        std::uint64_t frame_offset) const noexcept;

private:
    const int m_sample_rate;
    const int m_channels;

    const std::size_t m_capacity_frames;
    const std::size_t m_max_gap_fill_frames;
    const std::size_t m_discontinuity_threshold_frames;

    /*
     * 固定容量的交错Float32 PCM：
     *
     *     frame0_ch0, frame0_ch1,
     *     frame1_ch0, frame1_ch1, ...
     */
    std::vector<float> m_buffer;

    // 环形缓冲区中的位置以“每声道采样帧”为单位
    std::size_t m_read_position_frames{0};
    std::size_t m_write_position_frames{0};
    std::size_t m_available_frames{0};

    /*
     * 时间线锚点：
     *
     * m_timeline_anchor_us对应逻辑frame offset 0。
     * read/write offset持续递增，避免每次消费数据时累计微秒舍入误差。
     */
    bool m_timeline_initialized{false};
    std::int64_t m_timeline_anchor_us{0};

    std::uint64_t m_read_frame_offset{0};
    std::uint64_t m_write_frame_offset{0};

    mutable std::mutex m_mutex;
};

} // namespace RENDER