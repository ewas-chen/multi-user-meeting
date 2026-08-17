#pragma once

#include "TransportDefine.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace TRANSPORT {

/**
 * @brief 保守型视频码率控制参数
 *
 * 第一版只根据发送端的视频队列压力和发送失败进行调整，
 * 不依赖尚未接入的RTCP丢包率或带宽估计。
 */
struct AdaptiveVideoBitrateConfig final {
    std::uint32_t min_bitrate_kbps{300};
    std::uint32_t initial_bitrate_kbps{1200};
    std::uint32_t max_bitrate_kbps{1200};

    // 发生压力时按百分比降低，默认每次降低15%
    std::uint32_t decrease_percent{15};

    // 稳定恢复时采用固定步长，避免码率快速反弹
    std::uint32_t increase_step_kbps{80};

    // 连续稳定达到该时长后才允许提高一次码率
    std::uint32_t stable_duration_before_increase_ms{8000};

    // 降码率后暂停恢复一段时间，观察发送链路是否稳定
    std::uint32_t post_decrease_hold_ms{3000};

    // 队列达到该深度时认为发送链路存在持续压力
    std::size_t congested_queue_depth{2};

    // 队列不高于该深度且没有异常时才累计稳定时间
    std::size_t stable_queue_depth{1};
};

/**
 * @brief 一个统计周期内的视频发送反馈
 *
 * RtcPushTransport负责收集数据并定期提交给控制器。
 * 所有计数都是当前统计周期的增量，不是历史累计值。
 */
struct VideoBitrateFeedback final {
    std::uint32_t sample_duration_ms{0};
    std::size_t queue_depth{0};
    std::uint32_t queue_drop_count{0};
    std::uint32_t send_failure_count{0};
};

/**
 * @brief 保守型视频码率控制器
 *
 * 控制策略：
 * - 队列积压、队列丢帧或发送失败时快速降低码率；
 * - 链路持续稳定后缓慢恢复码率；
 * - 码率始终限制在配置范围内；
 * - 不直接访问编码器和RTC Track，保持策略与执行分离。
 *
 * 当前控制器由视频发送工作线程调用，因此内部不加锁。
 */
class TRANSPORT_ENGINE_LOCAL AdaptiveVideoBitrateController final {
public:
    AdaptiveVideoBitrateController() = default;
    ~AdaptiveVideoBitrateController() = default;

    /**
     * @brief 初始化控制器
     *
     * initial_bitrate_kbps必须位于最小值和最大值之间。
     */
    bool Initialize(const AdaptiveVideoBitrateConfig& config) noexcept;

    /**
     * @brief 重置运行状态并恢复到初始码率
     *
     * 用于重新推流、连接重建或编码器重新初始化。
     */
    void Reset() noexcept;

    /**
     * @brief 根据一个统计周期的反馈更新目标码率
     *
     * 返回新的码率表示需要调用编码器SetBitrate()；
     * 返回std::nullopt表示本周期保持当前码率。
     */
    std::optional<std::uint32_t> Update(const VideoBitrateFeedback& feedback) noexcept;

    [[nodiscard]]
    bool IsInitialized() const noexcept {
        return m_initialized;
    }

    [[nodiscard]]
    std::uint32_t GetCurrentBitrateKbps() const noexcept {
        return m_current_bitrate_kbps;
    }

private:
    bool IsCongested(const VideoBitrateFeedback& feedback) const noexcept;
    bool IsStable(const VideoBitrateFeedback& feedback) const noexcept;
    std::uint32_t CalculateReducedBitrate() const noexcept;

private:
    AdaptiveVideoBitrateConfig m_config{};
    std::uint32_t m_current_bitrate_kbps{0};
    std::uint64_t m_stable_duration_ms{0};
    std::uint64_t m_hold_remaining_ms{0};
    bool m_initialized{false};
};

} // namespace TRANSPORT