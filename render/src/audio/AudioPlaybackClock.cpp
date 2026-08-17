#include "AudioPlaybackClock.h"

namespace RENDER {

void AudioPlaybackClock::Update(std::int64_t timestamp_us) noexcept {
    // 媒体时间戳必须为正数，避免异常帧破坏已经建立的播放时钟。
    if (timestamp_us <= 0) {
        return;
    }

    // Release/Acquire保证其他线程读取到新时钟时，也能观察到更新前完成的状态修改。
    m_timestamp_us.store(timestamp_us, std::memory_order_release);
}

std::optional<std::int64_t> AudioPlaybackClock::GetTimestampUs() const noexcept {
    const std::int64_t timestamp_us = m_timestamp_us.load(std::memory_order_acquire);

    if (timestamp_us == kInvalidTimestampUs) {
        return std::nullopt;
    }

    return timestamp_us;
}

void AudioPlaybackClock::Reset() noexcept {
    // Reset后视频同步逻辑应暂时降级，直到音频回调再次更新时间戳。
    m_timestamp_us.store(kInvalidTimestampUs, std::memory_order_release);
}

} // namespace RENDER
