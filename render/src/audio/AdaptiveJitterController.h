#pragma once

#include "RenderDefine.h"

#include <cstdint>
#include <mutex>

namespace RENDER {

/**
 * @brief 自适应音频抖动缓冲配置
 *
 * 所有时间参数均使用微秒，与现有AudioFrame时间戳单位一致。
 */
struct AdaptiveJitterConfig {
    // 网络稳定时允许达到的最低目标缓存，默认60ms
    std::int64_t min_target_buffer_us{60'000};

    // 首次启动或时间线重建后的目标缓存，默认120ms
    std::int64_t initial_target_buffer_us{120'000};

    // 弱网情况下允许达到的最大目标缓存，默认250ms
    std::int64_t max_target_buffer_us{250'000};

    // 目标缓存中的固定安全余量，默认20ms
    std::int64_t safety_margin_us{20'000};

    // 目标缓存约为：最低缓存 + 安全余量 + jitter_multiplier × 抖动
    std::uint32_t jitter_multiplier{4};

    // 目标增幅小于该值时不调整，避免频繁波动
    std::int64_t adjustment_hysteresis_us{5'000};

    // 每次播放欠载后至少增加的目标缓存，默认30ms
    std::int64_t underrun_increase_us{30'000};

    // 迟到帧出现时增加的目标缓存，默认10ms
    std::int64_t late_frame_increase_us{10'000};

    // 连续稳定播放达到该时长后才允许降低缓存，默认5秒
    std::int64_t stable_window_us{5'000'000};

    // 每次稳定缩容最多减少的缓存，默认5ms
    std::int64_t decrease_step_us{5'000};
};

/**
 * @brief 自适应音频抖动缓冲运行统计
 *
 * 该结构只保存策略层统计，不重复保存PCM缓冲区已有状态。
 */
struct AdaptiveJitterStatistics {
    // 已参与抖动估算的有效到达样本数
    std::uint64_t arrival_sample_count{0};

    // 因时间戳无效、重复或回退而未参与估算的样本数
    std::uint64_t ignored_arrival_count{0};

    // 播放回调发生有效音频欠载的次数
    std::uint64_t underrun_count{0};

    // 已经过播放位置的迟到音频帧数
    std::uint64_t late_frame_count{0};

    // 目标缓存增加次数
    std::uint64_t target_increase_count{0};

    // 目标缓存降低次数
    std::uint64_t target_decrease_count{0};

    // 控制器时间线重置次数
    std::uint64_t reset_count{0};

    // 最近一次到达间隔偏差
    std::int64_t latest_jitter_sample_us{0};

    // 平滑后的网络抖动估算
    std::int64_t estimated_jitter_us{0};

    // 运行期间观察到的最大单次到达间隔偏差
    std::int64_t maximum_jitter_sample_us{0};

    // 当前动态目标缓存
    std::int64_t target_buffer_us{0};
};

/**
 * @brief PCM层自适应音频抖动控制器
 *
 * 本类只负责：
 * - 根据媒体时间间隔与本地到达间隔估算网络抖动；
 * - 计算动态目标缓存深度；
 * - 根据欠载、迟到和稳定播放反馈调整目标缓存。
 *
 * 本类不保存PCM、不读取TimestampedAudioBuffer，也不维护播放时钟。
 * AudioMixer负责把目标缓存时长转换为采样帧数，并复用现有
 * TimestampedAudioBuffer完成实际PCM存储和时间戳读取。
 *
 * 每个远端用户应拥有独立控制器，避免一个用户的网络抖动
 * 影响其他用户的缓存策略。
 */
class RENDER_ENGINE_LOCAL AdaptiveJitterController final {
public:
    explicit AdaptiveJitterController(
        const AdaptiveJitterConfig& config = {});

    ~AdaptiveJitterController() = default;

    AdaptiveJitterController(
        const AdaptiveJitterController&) = delete;

    AdaptiveJitterController& operator=(
        const AdaptiveJitterController&) = delete;

    AdaptiveJitterController(
        AdaptiveJitterController&&) = delete;

    AdaptiveJitterController& operator=(
        AdaptiveJitterController&&) = delete;

    /**
     * @brief 记录一个音频帧的媒体时间和本地到达时间
     *
     * @param media_timestamp_us 音频帧在公共媒体时间线上的时间戳
     * @param arrival_timestamp_us 使用steady_clock取得的本地到达时间
     *
     * 两个时间戳不需要拥有相同原点，只比较相邻帧的时间差：
     *
     * jitter_sample =
     *     abs(arrival_delta - media_delta)
     *
     * 平滑算法采用1/16权重，降低单次调度波动的影响。
     */
    void ObserveArrival(
        std::int64_t media_timestamp_us,
        std::int64_t arrival_timestamp_us) noexcept;

    /**
     * @brief 通知控制器发生播放欠载
     *
     * @param missing_duration_us 本次缺少有效PCM的持续时间
     *
     * 欠载发生后快速提高目标缓存，并重新开始稳定计时。
     */
    void NotifyUnderrun(
        std::int64_t missing_duration_us) noexcept;

    /**
     * @brief 通知控制器收到已经过播放位置的迟到帧
     *
     * 与TimestampedAudioPushResult::kDroppedLate配合使用，
     * 不在控制器内部重复判断PCM帧是否过期。
     */
    void NotifyLateFrame() noexcept;

    /**
     * @brief 通知控制器完成一段无欠载的稳定播放
     *
     * @param played_duration_us 本次稳定输出的有效PCM时长
     *
     * 只有累计稳定时间达到stable_window_us后，
     * 才允许逐步降低目标缓存。
     */
    void NotifyStablePlayback(
        std::int64_t played_duration_us) noexcept;

    /**
     * @brief 重置到达时间基准和动态缓存状态
     *
     * 用于用户重连、媒体时间戳跳变或整体播放时间线重建。
     * 重置后目标缓存恢复到initial_target_buffer_us。
     */
    void Reset() noexcept;

    /**
     * @brief 获取当前目标缓存时长
     *
     * AudioMixer使用该值判断用户PCM是否达到启动或
     * 重新缓冲深度。
     */
    [[nodiscard]]
    std::int64_t GetTargetBufferDurationUs() const noexcept;

    [[nodiscard]]
    AdaptiveJitterStatistics GetStatistics() const noexcept;

    [[nodiscard]]
    AdaptiveJitterConfig GetConfig() const noexcept;

private:
    static bool IsConfigValid(
        const AdaptiveJitterConfig& config) noexcept;

    // 根据当前平滑抖动计算理论目标缓存，调用时必须持有m_mutex
    std::int64_t CalculateDesiredTargetLocked() const noexcept;

    // 快速增大目标缓存，调用时必须持有m_mutex
    void IncreaseTargetLocked(
        std::int64_t requested_target_us) noexcept;

    // 稳定播放后缓慢降低目标缓存，调用时必须持有m_mutex
    void DecreaseTargetLocked() noexcept;

    // 将目标缓存限制在配置上下限内
    std::int64_t ClampTarget(
        std::int64_t target_us) const noexcept;

private:
    AdaptiveJitterConfig m_config;
    AdaptiveJitterStatistics m_statistics;

    bool m_arrival_baseline_initialized{false};
    std::int64_t m_last_media_timestamp_us{0};
    std::int64_t m_last_arrival_timestamp_us{0};

    // 连续无欠载播放时长，达到稳定窗口后才允许缩容
    std::int64_t m_stable_playback_duration_us{0};

    mutable std::mutex m_mutex;
};

} // namespace RENDER