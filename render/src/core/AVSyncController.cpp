#include "AVSyncController.h"

#include <algorithm>
#include <cstdlib>

namespace RENDER {

AVSyncController::AVSyncController(const AVSyncConfig& config)
{
    if (IsConfigValid(config)) {
        m_config = config;
    }
}

bool AVSyncController::SetConfig(const AVSyncConfig& config)
{
    if (!IsConfigValid(config)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_config = config;

    /*
     * 同步阈值或者首帧对齐策略改变后，原有时间基准不再可靠。
     * 这里只重置时间线，不清空累计统计。
     */
    m_timeline_initialized = false;
    m_timeline_offset_us = 0;
    m_last_video_timestamp_us = 0;
    m_last_audio_clock_us = 0;
    m_last_raw_delta_us = 0;
    m_has_smoothed_delta = false;

    return true;
}

AVSyncConfig AVSyncController::GetConfig() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

AVSyncDecision AVSyncController::EvaluateVideoFrame(
    std::int64_t video_timestamp_us,
    std::optional<std::int64_t> audio_clock_us)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    AVSyncDecision decision;

    /*
     * 音频尚未开始播放、纯视频会议或者播放时钟刚刚重建时，
     * 视频先直接显示，避免因为等待不存在的主时钟而停住。
     */
    if (!audio_clock_us) {
        decision.action = AVSyncAction::kRender;
        decision.synchronized = false;

        UpdateStatisticsLocked(decision);
        return decision;
    }

    decision.synchronized = true;

    const std::int64_t current_audio_clock_us =
        *audio_clock_us;

    const std::int64_t raw_delta_us =
        video_timestamp_us - current_audio_clock_us;

    if (!m_timeline_initialized) {
        /*
         * 第一次取得音频播放时钟时建立同步基准。
         *
         * 当前Transport的音视频时间戳可能分别由首个RTP包
         * 映射得到，因此默认消除第一组时间戳之间的固定偏移。
         */
        ResetTimelineLocked(
            video_timestamp_us,
            current_audio_clock_us);
    } else {
        const bool video_moved_backward =
            video_timestamp_us < m_last_video_timestamp_us &&
            m_last_video_timestamp_us - video_timestamp_us >
                m_config.backward_jump_threshold_us;

        const bool audio_moved_backward =
            current_audio_clock_us < m_last_audio_clock_us &&
            m_last_audio_clock_us - current_audio_clock_us >
                m_config.backward_jump_threshold_us;

        const std::int64_t delta_change_us =
            raw_delta_us >= m_last_raw_delta_us
                ? raw_delta_us - m_last_raw_delta_us
                : m_last_raw_delta_us - raw_delta_us;

        const bool delta_discontinuity =
            delta_change_us >
            m_config.discontinuity_threshold_us;

        /*
         * 时间戳明显回退或音视频差值突然大幅变化，通常表示：
         * - RTC重新连接；
         * - 音频播放设备发生切换；
         * - RTP时间戳重新开始；
         * - 媒体源发生切换。
         *
         * 继续使用旧基准会导致视频持续等待或持续丢弃，
         * 因此需要在当前帧重新建立时间线。
         */
        if (video_moved_backward ||
            audio_moved_backward ||
            delta_discontinuity) {
            ResetTimelineLocked(
                video_timestamp_us,
                current_audio_clock_us);

            decision.timeline_reset = true;
            ++m_statistics.timeline_reset_count;
        }
    }

    const std::int64_t corrected_delta_us =
        raw_delta_us - m_timeline_offset_us;

    decision.av_delta_us = corrected_delta_us;

    if (corrected_delta_us >
        m_config.video_early_threshold_us) {
        /*
         * 视频时间位于音频播放时钟的未来。
         * 保留当前视频帧，等待音频时钟追上。
         */
        decision.action = AVSyncAction::kWait;
    } else if (corrected_delta_us <
               -m_config.video_late_threshold_us) {
        /*
         * 视频已经明显落后于声音。
         * 丢弃当前帧，让视频队列继续追赶音频。
         */
        decision.action = AVSyncAction::kDrop;
    } else {
        decision.action = AVSyncAction::kRender;
    }

    m_last_video_timestamp_us = video_timestamp_us;
    m_last_audio_clock_us = current_audio_clock_us;
    m_last_raw_delta_us = raw_delta_us;

    UpdateStatisticsLocked(decision);
    return decision;
}

void AVSyncController::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_timeline_initialized = false;
    m_timeline_offset_us = 0;

    m_last_video_timestamp_us = 0;
    m_last_audio_clock_us = 0;
    m_last_raw_delta_us = 0;

    /*
     * 时间线重建后，旧时间线的平滑差值已经没有参考意义。
     * 累计计数仍然保留。
     */
    m_has_smoothed_delta = false;
}

AVSyncStatistics AVSyncController::GetStatistics() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_statistics;
}

void AVSyncController::ResetStatistics()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_statistics = {};
    m_has_smoothed_delta = false;
}

bool AVSyncController::IsTimelineInitialized() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_timeline_initialized;
}

bool AVSyncController::IsConfigValid(
    const AVSyncConfig& config) noexcept
{
    if (config.video_early_threshold_us < 0 ||
        config.video_late_threshold_us < 0 ||
        config.discontinuity_threshold_us <= 0 ||
        config.backward_jump_threshold_us <= 0) {
        return false;
    }

    /*
     * 跳变阈值必须大于正常同步区间，
     * 否则普通的早到或晚到帧也会频繁重置时间线。
     */
    const std::int64_t maximum_sync_threshold =
        std::max(
            config.video_early_threshold_us,
            config.video_late_threshold_us);

    return config.discontinuity_threshold_us >
           maximum_sync_threshold;
}

void AVSyncController::ResetTimelineLocked(
    std::int64_t video_timestamp_us,
    std::int64_t audio_clock_us) noexcept
{
    const std::int64_t raw_delta_us =
        video_timestamp_us - audio_clock_us;

    /*
     * 当前Transport还没有使用RTCP SR建立严格公共时间线，
     * 因而默认将第一组音视频时间戳之间的差值作为固定偏移。
     *
     * 如果音频和视频已经处于同一公共时间线，则不消除偏移。
     */
    m_timeline_offset_us =
        m_config.align_first_frame
            ? raw_delta_us
            : 0;

    m_last_video_timestamp_us = video_timestamp_us;
    m_last_audio_clock_us = audio_clock_us;
    m_last_raw_delta_us = raw_delta_us;

    m_timeline_initialized = true;
    m_has_smoothed_delta = false;
}

void AVSyncController::UpdateStatisticsLocked(
    const AVSyncDecision& decision) noexcept
{
    ++m_statistics.decision_count;

    switch (decision.action) {
    case AVSyncAction::kWait:
        ++m_statistics.wait_decision_count;
        break;

    case AVSyncAction::kRender:
        ++m_statistics.render_decision_count;
        break;

    case AVSyncAction::kDrop:
        ++m_statistics.dropped_frame_count;
        break;
    }

    if (!decision.synchronized) {
        ++m_statistics.unsynchronized_render_count;
        return;
    }

    m_statistics.current_av_delta_us =
        decision.av_delta_us;

    if (!m_has_smoothed_delta) {
        m_statistics.smoothed_av_delta_us =
            decision.av_delta_us;

        m_has_smoothed_delta = true;
    } else {
        /*
         * 使用1/8权重的指数移动平均，降低网络抖动造成的
         * 瞬时变化，同时避免引入浮点计算。
         */
        m_statistics.smoothed_av_delta_us +=
            (decision.av_delta_us -
             m_statistics.smoothed_av_delta_us) /
            8;
    }

    const std::int64_t absolute_delta_us =
        decision.av_delta_us >= 0
            ? decision.av_delta_us
            : -decision.av_delta_us;

    if (absolute_delta_us >
        m_statistics.maximum_abs_av_delta_us) {
        m_statistics.maximum_abs_av_delta_us =
            absolute_delta_us;
    }
}

} // namespace RENDER