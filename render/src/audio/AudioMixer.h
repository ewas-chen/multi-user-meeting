#pragma once

#include "AdaptiveJitterController.h"
#include "AudioPlaybackClock.h"
#include "IAudioSource.h"
#include "RenderDefine.h"
#include "TimestampedAudioBuffer.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace RENDER {

/**
 * @brief 多用户Float32 PCM音频混音器
 *
 * 数据流：
 * AudioFrame
 * -> 用户独立时间戳缓冲
 * -> 自适应启动缓存判断
 * -> 统一媒体时间线混音
 * -> MixedAudioChunk队列
 * -> AudioRender播放回调
 *
 * AudioMixer维护实际提交给播放设备的音频媒体时钟，
 * 该时钟作为远端视频同步的主时钟。
 */
class RENDER_ENGINE_LOCAL AudioMixer final : public IAudioSource {
public:
    AudioMixer();
    ~AudioMixer() override;

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) = delete;
    AudioMixer& operator=(AudioMixer&&) = delete;

    // 初始化混音器并启动混音线程
    bool Initialize(int sample_rate, int channels);

    void Uninitialize();

    /**
     * @brief 写入一个用户的交错Float32 PCM音频帧
     *
     * frame的采样率、声道数必须与AudioMixer初始化参数一致，
     * timestamp_us必须位于音视频共用的媒体时间线上。
     */
    bool PushAudioData(const std::string& user_name, const std::shared_ptr<AudioFrame>& frame);

    /**
     * @brief 向AudioRender播放回调提供混音结果
     *
     * 数据不足时输出静音。短暂欠载期间保持播放时钟连续，
     * 连续欠载超过阈值后停止旧时钟并重新进入启动缓存状态。
     */
    std::uint32_t PopAudio(float* output, std::uint32_t frame_count) noexcept override;

    // 删除指定用户的音频缓冲及抖动估算状态
    void RemoveUserBuffer(const std::string& user_name);

    // 提供给RenderEngine，作为远端视频同步的音频主时钟
    std::optional<std::int64_t> GetPlaybackTimestampUs() const noexcept;

    // 用户重连或单个用户时间戳跳变时重置其音频时间线
    void ResetUserTimeline(const std::string& user_name);

    // 声卡切换、整体重连或公共媒体时间线重建
    void ResetPlaybackTimeline();

private:
    /**
     * @brief 单个用户的PCM缓冲和抖动估算状态
     *
     * TimestampedAudioBuffer继续负责PCM存储和时间戳读取；
     * AdaptiveJitterController只负责计算启动缓存目标。
     */
    struct UserAudioState final {
        UserAudioState(int sample_rate,
                       int channels,
                       std::uint32_t max_buffer_duration_ms,
                       std::uint32_t max_gap_fill_duration_ms,
                       std::uint32_t discontinuity_threshold_ms)
            : buffer(sample_rate,
                     channels,
                     max_buffer_duration_ms,
                     max_gap_fill_duration_ms,
                     discontinuity_threshold_ms)
        {
        }

        TimestampedAudioBuffer buffer;
        AdaptiveJitterController jitter_controller;
    };

    // 获取或创建指定用户的音频状态
    std::shared_ptr<UserAudioState> GetOrCreateUserState(const std::string& user_name);

    // 获取用户状态快照，避免混音期间长期持有用户表互斥锁
    std::vector<std::shared_ptr<UserAudioState>> GetUserStatesSnapshot() const;

    /**
     * @brief 选择初始混音时间戳
     *
     * 只有缓冲深度达到各自动态启动目标的用户才参与选择，
     * 防止收到第一帧后立即播放造成频繁欠载。
     */
    std::optional<std::int64_t> FindInitialMixTimestampUs() const;

    /**
     * @brief 判断至少一个用户是否完整覆盖目标混音区间
     *
     * 不允许混音线程提前为尚未到达的未来音频生成静音块，
     * 否则混音游标会领先网络输入并将正常音频判定为过期。
     */
    bool HasBufferedAudioForChunk(std::int64_t chunk_timestamp_us) const;

    /**
     * @brief 判断混音线程当前是否可以生成下一块音频
     *
     * 调用时必须持有m_queue_mutex。
     */
    bool CanProduceMixChunk() const;

    /**
     * @brief 进入重新缓冲状态
     *
     * 清除尚未播放的预混音结果，停止旧播放时钟，
     * 等待音频达到当前启动缓存目标后建立新时间线。
     *
     * 调用时必须持有m_queue_mutex。
     */
    void EnterRebufferingLocked() noexcept;

    // 清理包含旧时间线数据的预混音结果
    void ClearMixedQueue();

    // 对混音结果进行幅度限制，防止Float32 PCM溢出
    static void ClampAndNormalize(float* samples, std::size_t sample_count) noexcept;

    static std::int64_t FramesToMicroseconds(std::uint64_t frames, int sample_rate) noexcept;
    static std::size_t MicrosecondsToFrames(std::int64_t duration_us, int sample_rate) noexcept;

    // 混音线程入口
    void MixingThreadLoop();

private:
    static constexpr std::size_t kPreMixedChunkCount{5};
    static constexpr std::uint32_t kMixChunkDurationMs{10};

    /*
     * 短暂欠载继续输出静音并保持时钟连续。
     * 连续欠载达到100ms后，旧播放时间线已不可靠，
     * 重新进入启动缓存状态。
     */
    static constexpr std::uint32_t kContinuousUnderrunRecoveryMs{100};

    static constexpr std::uint32_t kMaxUserBufferDurationMs{500};
    static constexpr std::uint32_t kMaxGapFillDurationMs{100};
    static constexpr std::uint32_t kDiscontinuityThresholdMs{500};

    int m_sample_rate{0};
    int m_channels{0};

    std::uint32_t m_mix_frame_count{0};
    std::uint32_t m_rebuffer_threshold_frames{0};

    /*
     * 每个远端用户独立维护PCM缓冲和抖动估算状态，
     * 多个用户最终在同一媒体时间区间内完成混音。
     */
    std::unordered_map<std::string, std::shared_ptr<UserAudioState>> m_user_states;
    mutable std::mutex m_user_states_mutex;

    /*
     * 所有用户均使用该游标调用ReadAt()，
     * 保证混音发生在相同媒体时间区间。
     */
    std::int64_t m_mix_cursor_us{0};
    bool m_mix_cursor_initialized{false};

    std::deque<MixedAudioChunk> m_mixed_queue;

    // 当前队首混音块已提交给播放设备的帧偏移
    std::size_t m_front_chunk_frame_offset{0};

    /*
     * 连续未从预混音队列取得的音频帧数。
     * 由m_queue_mutex保护，到达恢复阈值后重新缓冲。
     */
    std::uint64_t m_consecutive_underrun_frames{0};

    mutable std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;

    AudioPlaybackClock m_playback_clock;

    std::thread m_mixing_thread;
    std::atomic<bool> m_mixing_thread_running{false};
    std::atomic<bool> m_initialized{false};

    std::mutex m_state_mutex;
};

} // namespace RENDER