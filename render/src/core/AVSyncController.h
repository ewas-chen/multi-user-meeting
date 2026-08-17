#pragma once

#include "RenderDefine.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace RENDER {

/**
 * @brief 视频帧同步处理结果
 */
enum class AVSyncAction : std::uint8_t
{
    // 视频早于音频，暂时保留队首视频帧
    kWait = 0,

    // 音视频时间差处于允许范围，显示当前视频帧
    kRender,

    // 视频明显落后于音频，丢弃当前视频帧
    kDrop
};

/**
 * @brief 音视频同步参数
 *
 * 时间差定义：
 *
 *     av_delta_us = video_timestamp_us - audio_clock_us
 *
 * av_delta_us > 0：视频位于音频未来，需要等待。
 * av_delta_us < 0：视频已经落后于音频，可能需要丢帧追赶。
 */
struct AVSyncConfig
{
    // 视频比音频早超过该值时等待，默认50ms
    std::int64_t video_early_threshold_us{50'000};

    // 视频比音频晚超过该值时丢弃，默认80ms
    std::int64_t video_late_threshold_us{80'000};

    /*
     * 相邻两次音视频时间差变化超过该值时，
     * 认为发生断线重连、时间戳重置或媒体源切换。
     */
    std::int64_t discontinuity_threshold_us{500'000};

    /*
     * 时间戳向后跳变超过该值时重建同步基准。
     * 少量抖动不应该触发整个同步状态重置。
     */
    std::int64_t backward_jump_threshold_us{100'000};

    /*
     * 当前Transport尚未通过RTCP SR把音频和视频映射到
     * 完全相同的公共时间线，因此第一组音视频时间戳需要
     * 建立相对同步基准。
     *
     * 后续完成RTCP SR时间映射后可以设置为false，
     * 直接比较公共时间线上的音频和视频时间戳。
     */
    bool align_first_frame{true};
};

/**
 * @brief 单次视频帧同步决策
 */
struct AVSyncDecision
{
    AVSyncAction action{AVSyncAction::kRender};

    // 修正时间基准后的音视频时间差
    std::int64_t av_delta_us{0};

    /*
     * true表示本次决策使用了有效音频播放时钟。
     * false表示音频时钟尚未建立，视频采用直接渲染策略。
     */
    bool synchronized{false};

    /*
     * true表示本次检测到时间戳跳变，并重新建立了同步基准。
     * UserContext可以借此清除跳变前遗留的视频帧。
     */
    bool timeline_reset{false};
};

/**
 * @brief 音视频同步运行统计
 *
 * wait_decision_count统计的是等待决策次数，不是视频帧数。
 * 同一帧在等待期间可能被EvaluateVideoFrame检查多次。
 */
struct AVSyncStatistics
{
    std::uint64_t decision_count{0};

    std::uint64_t render_decision_count{0};
    std::uint64_t wait_decision_count{0};
    std::uint64_t dropped_frame_count{0};

    // 音频时钟不可用时直接渲染的次数
    std::uint64_t unsynchronized_render_count{0};

    // 时间戳跳变后重新建立同步基准的次数
    std::uint64_t timeline_reset_count{0};

    // 最近一次经过时间基准修正的音视频时间差
    std::int64_t current_av_delta_us{0};

    // 对音视频时间差进行平滑后的结果
    std::int64_t smoothed_av_delta_us{0};

    // 运行期间出现过的最大绝对音视频时间差
    std::int64_t maximum_abs_av_delta_us{0};
};

/**
 * @brief 基于音频播放时钟的视频同步控制器
 *
 * 该类只负责同步决策，不持有音频数据、视频帧或渲染资源。
 *
 * 推荐每个远端UserContext拥有一个独立控制器：
 *
 *     AudioRender播放时钟
 *              +
 *     当前用户视频时间戳
 *              |
 *              v
 *       AVSyncController
 *              |
 *       Wait / Render / Drop
 *
 * 本地摄像头预览不需要经过该控制器，应直接显示最新视频帧，
 * 以降低本地预览延迟。
 */
class RENDER_ENGINE_LOCAL AVSyncController final
{
public:
    explicit AVSyncController(const AVSyncConfig& config = {});

    ~AVSyncController() = default;

    AVSyncController(const AVSyncController&) = delete;
    AVSyncController& operator=(const AVSyncController&) = delete;
    AVSyncController(AVSyncController&&) = delete;
    AVSyncController& operator=(AVSyncController&&) = delete;

    /**
     * @brief 更新同步参数
     *
     * 参数无效时返回false，并保留原配置。
     * 更新成功后会重置当前同步时间基准，但保留统计信息。
     */
    bool SetConfig(const AVSyncConfig& config);

    AVSyncConfig GetConfig() const;

    /**
     * @brief 根据音频播放时钟判断当前视频帧如何处理
     *
     * @param video_timestamp_us 当前视频帧的媒体时间戳
     * @param audio_clock_us 当前扬声器播放到的媒体时间；
     *        没有有效音频时钟时传入std::nullopt
     *
     * 音频时钟不可用时返回kRender，并将synchronized设为false，
     * 从而支持纯视频会议以及音频尚未启动时的视频显示
     */
    AVSyncDecision EvaluateVideoFrame(
        std::int64_t video_timestamp_us,
        std::optional<std::int64_t> audio_clock_us);

    /**
     * @brief 重置音视频时间基准
     *
     * 用于用户重新订阅、音频设备切换、音频时钟重建等情况。
     * 不清空累计统计信息。
     */
    void Reset();

    /**
     * @brief 获取同步统计快照
     */
    AVSyncStatistics GetStatistics() const;

    /**
     * @brief 清空累计统计信息
     *
     * 不影响当前已经建立的同步时间基准。
     */
    void ResetStatistics();

    bool IsTimelineInitialized() const;

private:
    static bool IsConfigValid(const AVSyncConfig& config) noexcept;

    /**
     * @brief 使用当前音视频时间戳建立新的时间基准
     */
    void ResetTimelineLocked(
        std::int64_t video_timestamp_us,
        std::int64_t audio_clock_us) noexcept;

    /**
     * @brief 更新统计数据
     */
    void UpdateStatisticsLocked(
        const AVSyncDecision& decision) noexcept;

private:
    mutable std::mutex m_mutex;

    AVSyncConfig m_config;
    AVSyncStatistics m_statistics;

    bool m_timeline_initialized{false};

    /*
     * align_first_frame=true时：
     *
     * timeline_offset_us =
     *     first_video_timestamp_us - first_audio_clock_us
     *
     * 后续实际参与同步判断的时间差为：
     *
     * corrected_delta =
     *     video_timestamp_us
     *     - audio_clock_us
     *     - timeline_offset_us
     */
    std::int64_t m_timeline_offset_us{0};

    std::int64_t m_last_video_timestamp_us{0};
    std::int64_t m_last_audio_clock_us{0};
    std::int64_t m_last_raw_delta_us{0};

    // 是否已经获得用于平滑计算的第一组时间差
    bool m_has_smoothed_delta{false};
};

} // namespace RENDER