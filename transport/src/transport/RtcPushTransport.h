#pragma once

#include "AdaptiveVideoBitrateController.h"
#include "RtcTransportBase.h"

#include "codec/audio_codec/AudioCodecFactory.h"
#include "codec/video_codec/VideoCodecFactory.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace TRANSPORT {

/**
 * @brief 本地音视频RTC推流实现
 *
 * 主要链路：
 *
 * I420Frame
 *   -> 视频队列
 *   -> H.264编码
 *   -> RTP封包
 *   -> 视频Track
 *
 * AudioFrame
 *   -> 音频队列
 *   -> Opus编码
 *   -> RTP封包
 *   -> 音频Track
 *
 * PushVideoFrame()和PushAudioFrame()只负责将帧放入有界队列，
 * 不在OBS采集回调线程中执行耗时的编码和网络发送。
 */
class TRANSPORT_ENGINE_LOCAL RtcPushTransport final : public RtcTransportBase {
public:
    RtcPushTransport();
    ~RtcPushTransport() override;

    RtcPushTransport(const RtcPushTransport&) = delete;
    RtcPushTransport& operator=(const RtcPushTransport&) = delete;
    RtcPushTransport(RtcPushTransport&&) = delete;
    RtcPushTransport& operator=(RtcPushTransport&&) = delete;

    /**
     * @brief 开始发布视频
     *
     * 首次启动音频或视频时会统一创建两个发送Track，
     * 保证生成的Offer SDP中同时包含音频和视频描述。
     */
    bool StartPublishVideo(const std::string& cname);

    // 停止视频发布并释放视频编码器和工作线程
    bool StopPublishVideo();

    // 开始发布音频
    bool StartPublishAudio(const std::string& cname);

    // 停止音频发布并释放音频编码器和工作线程
    bool StopPublishAudio();

    /**
     * @brief 将采集模块输出的I420帧放入视频编码队列
     *
     * 队列满时丢弃最旧的视频帧，优先保证低延迟。
     */
    bool PushVideoFrame(const std::shared_ptr<I420Frame>& frame);

    /**
     * @brief 将采集模块输出的PCM帧放入音频编码队列
     *
     * 音频队列同样有容量限制，避免网络异常时延迟无限增长。
     */
    bool PushAudioFrame(const std::shared_ptr<AudioFrame>& frame);

    bool IsPublishingVideo() const noexcept;
    bool IsPublishingAudio() const noexcept;

    /**
     * @brief 关闭推流对象
     *
     * 关闭顺序：
     * 1. 停止接收新帧。
     * 2. 停止音视频编码线程。
     * 3. 释放编码器。
     * 4. 关闭音视频Track。
     * 5. 关闭PeerConnection。
     */
    void Close() override;

private:
    // 创建音视频发送Track，并配置RTP/RTCP处理链
    bool SetupPublishTracks(const std::string& cname);

    bool InitializeVideoEncoder();
    bool InitializeAudioEncoder();

    /**
     * @brief 提交一个周期的视频发送反馈并应用新码率
     *
     * 只由视频工作线程调用。queue_depth是在取出当前帧后，
     * 视频队列中仍然等待编码的帧数。
     */
    void EvaluateVideoBitrate(std::uint32_t sample_duration_ms, std::size_t queue_depth);

    // 清除上一轮推流留下的反馈计数
    void ResetVideoBitrateFeedback() noexcept;

    // 从队列读取原始帧，完成编码并通过Track发送
    void VideoWorkerLoop();
    void AudioWorkerLoop();

private:
    /*
     * 视频只保留少量待编码帧。
     * 编码跟不上采集速度时，继续处理旧帧只会不断增加延迟。
     */
    static constexpr std::size_t kMaxVideoQueueSize = 3;

    // 每秒评估一次，避免频繁修改OpenH264运行参数
    static constexpr std::uint32_t kVideoBitrateEvaluationIntervalMs = 1000;

    /*
     * 20ms Opus帧时，10个缓存约为200ms。
     * 这是防止异常积压的上限，不是目标延迟。
     */
    static constexpr std::size_t kMaxAudioQueueSize = 10;

    std::atomic<bool> m_video_publishing{false};
    std::atomic<bool> m_audio_publishing{false};

    std::shared_ptr<rtc::Track> m_video_track;
    std::shared_ptr<rtc::Track> m_audio_track;

    mutable std::mutex m_tracks_mutex;
    bool m_tracks_initialized{false};

    std::unique_ptr<CODEC::IVideoEncoder> m_video_encoder;
    std::unique_ptr<CODEC::IAudioEncoder> m_audio_encoder;

    /*
     * 控制器只计算目标码率，不直接访问编码器。
     * InitializeVideoEncoder()负责使用同一组码率上下限初始化二者。
     */
    AdaptiveVideoBitrateController m_video_bitrate_controller;

    /*
     * PLI回调可能要求视频编码器立即产生关键帧，
     * 视频线程也会执行编码和动态码率设置，因此共用该锁。
     */
    mutable std::mutex m_video_encoder_mutex;
    mutable std::mutex m_audio_encoder_mutex;

    std::deque<std::shared_ptr<I420Frame>> m_video_queue;
    std::mutex m_video_queue_mutex;
    std::condition_variable m_video_queue_cv;
    std::thread m_video_worker;

    /*
     * PushVideoFrame运行在采集回调线程，视频工作线程定期读取并清零。
     * 使用成员原子量，避免头文件全局统计产生多个独立副本。
     */
    std::atomic<std::uint32_t> m_video_queue_drop_count{0};
    std::atomic<std::uint32_t> m_video_send_failure_count{0};

    std::deque<std::shared_ptr<AudioFrame>> m_audio_queue;
    std::mutex m_audio_queue_mutex;
    std::condition_variable m_audio_queue_cv;
    std::thread m_audio_worker;

    /*
     * 音视频共用同一个会话起始时间，
     * 后续分别映射为视频90kHz和音频48kHz RTP时间戳。
     */
    std::atomic<std::int64_t> m_session_start_time_us{0};
};

} // namespace TRANSPORT