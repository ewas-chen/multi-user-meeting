#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace RENDER {

/**
 * @brief 记录音频设备当前消费到的媒体时间
 *
 * AudioMixer在播放回调消费混音数据后更新时间戳，视频渲染线程通过
 * GetTimestampUs()读取该时间戳，并将其作为音视频同步的主时钟。
 */
class AudioPlaybackClock final {
public:
    AudioPlaybackClock() = default;

    AudioPlaybackClock(const AudioPlaybackClock&) = delete;
    AudioPlaybackClock& operator=(const AudioPlaybackClock&) = delete;

    // 更新时间戳。无效时间戳不会覆盖当前有效时钟。
    void Update(std::int64_t timestamp_us) noexcept;

    // 尚未建立播放时间线时返回std::nullopt。
    std::optional<std::int64_t> GetTimestampUs() const noexcept;

    // 在设备切换、重连或媒体时间线重建时清除当前时钟。
    void Reset() noexcept;

private:
    static constexpr std::int64_t kInvalidTimestampUs = -1;

    std::atomic<std::int64_t> m_timestamp_us{kInvalidTimestampUs};
};

} // namespace RENDER