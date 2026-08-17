#include "AdaptiveJitterController.h"

#include <algorithm>
#include <limits>

namespace RENDER {

namespace {

std::int64_t SaturatingAdd(
    std::int64_t left,
    std::int64_t right) noexcept
{
    if (right > 0 &&
        left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }

    if (right < 0 &&
        left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }

    return left + right;
}

} // namespace

AdaptiveJitterController::AdaptiveJitterController(
    const AdaptiveJitterConfig& config)
{
    /*
     * 无效配置不应使控制器进入不可用状态，
     * 因此自动回退到默认参数。
     */
    if (IsConfigValid(config)) {
        m_config = config;
    } else {
        m_config = AdaptiveJitterConfig{};
    }

    m_statistics.target_buffer_us =
        m_config.initial_target_buffer_us;
}

void AdaptiveJitterController::ObserveArrival(
    std::int64_t media_timestamp_us,
    std::int64_t arrival_timestamp_us) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (media_timestamp_us <= 0 ||
        arrival_timestamp_us <= 0) {
        ++m_statistics.ignored_arrival_count;
        return;
    }

    /*
     * 第一帧只用于建立媒体时间和本地到达时间基准。
     * 从第二帧开始才能计算相邻间隔差。
     */
    if (!m_arrival_baseline_initialized) {
        m_last_media_timestamp_us =
            media_timestamp_us;

        m_last_arrival_timestamp_us =
            arrival_timestamp_us;

        m_arrival_baseline_initialized = true;
        return;
    }

    /*
     * 媒体时间戳重复或回退时不参与抖动估算。
     *
     * 本地到达时间来自steady_clock，理论上不会回退；
     * 如果仍然检测到回退，也忽略当前样本，防止污染估算。
     */
    if (media_timestamp_us <=
            m_last_media_timestamp_us ||
        arrival_timestamp_us <
            m_last_arrival_timestamp_us) {
        ++m_statistics.ignored_arrival_count;
        return;
    }

    const std::int64_t media_interval_us =
        media_timestamp_us -
        m_last_media_timestamp_us;

    const std::int64_t arrival_interval_us =
        arrival_timestamp_us -
        m_last_arrival_timestamp_us;

    /*
     * 有效样本已经消费，因此立即更新基准。
     */
    m_last_media_timestamp_us =
        media_timestamp_us;

    m_last_arrival_timestamp_us =
        arrival_timestamp_us;

    const std::int64_t jitter_sample_us =
        arrival_interval_us >= media_interval_us
            ? arrival_interval_us - media_interval_us
            : media_interval_us - arrival_interval_us;

    ++m_statistics.arrival_sample_count;

    m_statistics.latest_jitter_sample_us =
        jitter_sample_us;

    m_statistics.maximum_jitter_sample_us =
        std::max(
            m_statistics.maximum_jitter_sample_us,
            jitter_sample_us);

    /*
     * 使用1/16权重的指数移动平均：
     *
     * estimated += (sample - estimated) / 16
     *
     * 它能降低单次线程调度波动对目标缓存的影响，
     * 同时仍能跟踪持续性的网络抖动变化。
     */
    m_statistics.estimated_jitter_us +=
        (jitter_sample_us -
         m_statistics.estimated_jitter_us) / 16;

    const std::int64_t desired_target_us =
        CalculateDesiredTargetLocked();

    /*
     * 到达抖动增大时允许立即提高目标缓存。
     * 缓存降低只能由稳定播放反馈触发。
     */
    IncreaseTargetLocked(
        desired_target_us);
}

void AdaptiveJitterController::NotifyUnderrun(
    std::int64_t missing_duration_us) noexcept
{
    if (missing_duration_us <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    ++m_statistics.underrun_count;

    /*
     * 任何欠载都会中断稳定播放状态。
     * 即使目标缓存已经达到上限，也必须重新累计稳定窗口。
     */
    m_stable_playback_duration_us = 0;

    const std::int64_t increase_us =
        std::max(
            m_config.underrun_increase_us,
            missing_duration_us);

    const std::int64_t requested_target_us =
        SaturatingAdd(
            m_statistics.target_buffer_us,
            increase_us);

    IncreaseTargetLocked(
        requested_target_us);
}

void AdaptiveJitterController::NotifyLateFrame() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    ++m_statistics.late_frame_count;

    /*
     * 迟到帧说明当前播放位置过于靠近网络输入边缘，
     * 小幅提高目标缓存并重新开始稳定计时。
     */
    m_stable_playback_duration_us = 0;

    const std::int64_t requested_target_us =
        SaturatingAdd(
            m_statistics.target_buffer_us,
            m_config.late_frame_increase_us);

    IncreaseTargetLocked(
        requested_target_us);
}

void AdaptiveJitterController::NotifyStablePlayback(
    std::int64_t played_duration_us) noexcept
{
    if (played_duration_us <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    /*
     * 使用饱和加法，避免长时间运行后累计时长溢出。
     */
    m_stable_playback_duration_us =
        SaturatingAdd(
            m_stable_playback_duration_us,
            played_duration_us);

    /*
     * 每经过一个完整稳定窗口最多降低一步。
     * 如果一次反馈跨越多个稳定窗口，可以依次降低多步。
     */
    while (m_stable_playback_duration_us >=
           m_config.stable_window_us) {
        m_stable_playback_duration_us -=
            m_config.stable_window_us;

        const std::int64_t previous_target_us =
            m_statistics.target_buffer_us;

        DecreaseTargetLocked();

        /*
         * 当前目标已经达到抖动估算要求，
         * 无需继续保留多余的稳定时长。
         */
        if (m_statistics.target_buffer_us ==
            previous_target_us) {
            m_stable_playback_duration_us = 0;
            break;
        }
    }
}

void AdaptiveJitterController::Reset() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_arrival_baseline_initialized = false;
    m_last_media_timestamp_us = 0;
    m_last_arrival_timestamp_us = 0;
    m_stable_playback_duration_us = 0;

    /*
     * 重置当前估算状态，但保留累计运行统计，
     * 便于诊断整个会话期间的网络表现。
     */
    m_statistics.latest_jitter_sample_us = 0;
    m_statistics.estimated_jitter_us = 0;
    m_statistics.target_buffer_us =
        m_config.initial_target_buffer_us;

    ++m_statistics.reset_count;
}

std::int64_t
AdaptiveJitterController::
GetTargetBufferDurationUs() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_statistics.target_buffer_us;
}

AdaptiveJitterStatistics
AdaptiveJitterController::GetStatistics() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_statistics;
}

AdaptiveJitterConfig
AdaptiveJitterController::GetConfig() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_config;
}

bool AdaptiveJitterController::IsConfigValid(
    const AdaptiveJitterConfig& config) noexcept
{
    if (config.min_target_buffer_us <= 0 ||
        config.initial_target_buffer_us <
            config.min_target_buffer_us ||
        config.max_target_buffer_us <
            config.initial_target_buffer_us) {
        return false;
    }

    const std::int64_t target_range_us =
        config.max_target_buffer_us -
        config.min_target_buffer_us;

    if (config.safety_margin_us < 0 ||
        config.safety_margin_us >
            target_range_us ||
        config.jitter_multiplier == 0 ||
        config.adjustment_hysteresis_us < 0 ||
        config.adjustment_hysteresis_us >
            target_range_us) {
        return false;
    }

    if (config.underrun_increase_us <= 0 ||
        config.late_frame_increase_us <= 0 ||
        config.stable_window_us <= 0 ||
        config.decrease_step_us <= 0) {
        return false;
    }

    return true;
}

std::int64_t
AdaptiveJitterController::
CalculateDesiredTargetLocked() const noexcept
{
    /*
     * 理论目标：
     *
     * minimum + safety margin +
     * jitter multiplier × estimated jitter
     */
    std::int64_t desired_target_us =
        SaturatingAdd(
            m_config.min_target_buffer_us,
            m_config.safety_margin_us);

    const std::int64_t estimated_jitter_us =
        std::max<std::int64_t>(
            m_statistics.estimated_jitter_us,
            0);

    std::int64_t jitter_margin_us = 0;

    if (estimated_jitter_us >
        std::numeric_limits<std::int64_t>::max() /
            static_cast<std::int64_t>(
                m_config.jitter_multiplier)) {
        jitter_margin_us =
            std::numeric_limits<std::int64_t>::max();
    } else {
        jitter_margin_us =
            estimated_jitter_us *
            static_cast<std::int64_t>(
                m_config.jitter_multiplier);
    }

    desired_target_us =
        SaturatingAdd(
            desired_target_us,
            jitter_margin_us);

    return ClampTarget(
        desired_target_us);
}

void AdaptiveJitterController::IncreaseTargetLocked(
    std::int64_t requested_target_us) noexcept
{
    const std::int64_t new_target_us =
        ClampTarget(requested_target_us);

    if (new_target_us <=
        m_statistics.target_buffer_us) {
        return;
    }

    const std::int64_t increase_us =
        new_target_us -
        m_statistics.target_buffer_us;

    /*
     * 小于滞回阈值的变化不立即应用，
     * 防止目标缓存随轻微抖动反复调整。
     */
    if (increase_us <
        m_config.adjustment_hysteresis_us) {
        return;
    }

    m_statistics.target_buffer_us =
        new_target_us;

    ++m_statistics.target_increase_count;

    /*
     * 目标缓存刚刚提高，稳定缩容计时需要重新开始。
     */
    m_stable_playback_duration_us = 0;
}

void AdaptiveJitterController::DecreaseTargetLocked() noexcept
{
    const std::int64_t desired_target_us =
        CalculateDesiredTargetLocked();

    if (m_statistics.target_buffer_us <=
        desired_target_us) {
        return;
    }

    const std::int64_t stepped_target_us =
        std::max(
            desired_target_us,
            m_statistics.target_buffer_us -
                m_config.decrease_step_us);

    const std::int64_t decrease_us =
        m_statistics.target_buffer_us -
        stepped_target_us;

    /*
     * 缩容变化小于滞回阈值时保持当前目标，
     * 避免目标在边界附近来回摆动。
     */
    if (decrease_us <
        m_config.adjustment_hysteresis_us) {
        return;
    }

    m_statistics.target_buffer_us =
        ClampTarget(stepped_target_us);

    ++m_statistics.target_decrease_count;
}

std::int64_t AdaptiveJitterController::ClampTarget(
    std::int64_t target_us) const noexcept
{
    return std::clamp(
        target_us,
        m_config.min_target_buffer_us,
        m_config.max_target_buffer_us);
}

} // namespace RENDER