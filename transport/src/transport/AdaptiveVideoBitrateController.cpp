#include "AdaptiveVideoBitrateController.h"

#include <algorithm>
#include <cstdint>

namespace TRANSPORT {

bool AdaptiveVideoBitrateController::Initialize(const AdaptiveVideoBitrateConfig& config) noexcept {
    const bool bitrate_valid =
        config.min_bitrate_kbps > 0 &&
        config.initial_bitrate_kbps >= config.min_bitrate_kbps &&
        config.initial_bitrate_kbps <= config.max_bitrate_kbps;

    const bool adjustment_valid =
        config.decrease_percent > 0 &&
        config.decrease_percent < 100 &&
        config.increase_step_kbps > 0 &&
        config.stable_duration_before_increase_ms > 0;

    const bool queue_threshold_valid =
        config.congested_queue_depth > config.stable_queue_depth;

    if (!bitrate_valid || !adjustment_valid || !queue_threshold_valid) {
        m_config = {};
        m_current_bitrate_kbps = 0;
        m_stable_duration_ms = 0;
        m_hold_remaining_ms = 0;
        m_initialized = false;
        return false;
    }

    m_config = config;
    m_current_bitrate_kbps = config.initial_bitrate_kbps;
    m_stable_duration_ms = 0;
    m_hold_remaining_ms = 0;
    m_initialized = true;
    return true;
}

void AdaptiveVideoBitrateController::Reset() noexcept {
    m_stable_duration_ms = 0;
    m_hold_remaining_ms = 0;

    if (m_initialized) {
        m_current_bitrate_kbps = m_config.initial_bitrate_kbps;
    }
}

std::optional<std::uint32_t> AdaptiveVideoBitrateController::Update(
    const VideoBitrateFeedback& feedback) noexcept {
    if (!m_initialized || feedback.sample_duration_ms == 0) {
        return std::nullopt;
    }

    /*
     * 队列丢帧、发送失败或持续积压都表示当前发送压力较高。
     * 压力存在时立即降低一级码率，并重新开始恢复等待。
     */
    if (IsCongested(feedback)) {
        m_stable_duration_ms = 0;
        m_hold_remaining_ms = m_config.post_decrease_hold_ms;

        const std::uint32_t reduced_bitrate = CalculateReducedBitrate();
        if (reduced_bitrate >= m_current_bitrate_kbps) {
            return std::nullopt;
        }

        m_current_bitrate_kbps = reduced_bitrate;
        return m_current_bitrate_kbps;
    }

    /*
     * 降码率后先观察一段时间。等待期间即使队列为空，
     * 也不立即恢复码率，避免在临界网络状态下反复升降。
     */
    if (m_hold_remaining_ms > 0) {
        if (feedback.sample_duration_ms >= m_hold_remaining_ms) {
            m_hold_remaining_ms = 0;
        } else {
            m_hold_remaining_ms -= feedback.sample_duration_ms;
        }

        m_stable_duration_ms = 0;
        return std::nullopt;
    }

    /*
     * 队列深度位于稳定阈值和拥塞阈值之间时保持码率，
     * 但不累计稳定时间。
     */
    if (!IsStable(feedback)) {
        m_stable_duration_ms = 0;
        return std::nullopt;
    }

    m_stable_duration_ms += feedback.sample_duration_ms;
    if (m_stable_duration_ms < m_config.stable_duration_before_increase_ms) {
        return std::nullopt;
    }

    m_stable_duration_ms = 0;

    if (m_current_bitrate_kbps >= m_config.max_bitrate_kbps) {
        return std::nullopt;
    }

    const std::uint64_t increased_bitrate =
        static_cast<std::uint64_t>(m_current_bitrate_kbps) +
        m_config.increase_step_kbps;

    m_current_bitrate_kbps = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(
            increased_bitrate,
            m_config.max_bitrate_kbps));

    return m_current_bitrate_kbps;
}

bool AdaptiveVideoBitrateController::IsCongested(
    const VideoBitrateFeedback& feedback) const noexcept {
    return feedback.queue_drop_count > 0 ||
           feedback.send_failure_count > 0 ||
           feedback.queue_depth >= m_config.congested_queue_depth;
}

bool AdaptiveVideoBitrateController::IsStable(
    const VideoBitrateFeedback& feedback) const noexcept {
    return feedback.queue_drop_count == 0 &&
           feedback.send_failure_count == 0 &&
           feedback.queue_depth <= m_config.stable_queue_depth;
}

std::uint32_t AdaptiveVideoBitrateController::CalculateReducedBitrate() const noexcept {
    if (m_current_bitrate_kbps <= m_config.min_bitrate_kbps) {
        return m_config.min_bitrate_kbps;
    }

    const std::uint64_t calculated_reduction =
        static_cast<std::uint64_t>(m_current_bitrate_kbps) *
        m_config.decrease_percent / 100;

    // 非常低的码率下也保证一次至少降低1kbps
    const std::uint32_t reduction =
        static_cast<std::uint32_t>(
            std::max<std::uint64_t>(calculated_reduction, 1));

    if (reduction >=
        m_current_bitrate_kbps - m_config.min_bitrate_kbps) {
        return m_config.min_bitrate_kbps;
    }

    return m_current_bitrate_kbps - reduction;
}

} // namespace TRANSPORT