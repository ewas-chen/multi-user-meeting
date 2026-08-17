#pragma once

#include "RtcTransportBase.h"
#include "RtpNtpTimeMapper.h"
#include "codec/audio_codec/AudioCodecFactory.h"
#include "codec/video_codec/VideoCodecFactory.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace TRANSPORT {

class VideoRtpReorderBuffer;
/**
 * @brief 远端用户音视频RTC拉流实现
 *
 * 视频接收链路：
 *
 * 视频Track
 *   -> RTP解包
 *   -> EncodedVideoFrame队列
 *   -> H.264解码线程
 *   -> I420Frame回调
 *
 * 音频接收链路：
 *
 * 音频Track
 *   -> RTP解包
 *   -> EncodedAudioFrame队列
 *   -> Opus解码线程
 *   -> AudioFrame回调
 *
 * libdatachannel回调中不直接执行解码，避免耗时解码阻塞
 * WebRTC内部网络线程。
 */
class TRANSPORT_ENGINE_LOCAL RtcPullTransport final
    : public RtcTransportBase {
public:
    RtcPullTransport();
    ~RtcPullTransport() override;

    RtcPullTransport(const RtcPullTransport&) = delete;
    RtcPullTransport& operator=(const RtcPullTransport&) = delete;
    RtcPullTransport(RtcPullTransport&&) = delete;
    RtcPullTransport& operator=(RtcPullTransport&&) = delete;

    /**
     * @brief 创建远端音视频接收Track并生成WHEP Offer
     * 开始订阅一个远端用户的音视频，是拉流端最主要的启动函数
     * 调用前必须完成RtcTransportBase::Initialize()。
     * 函数会创建RecvOnly音视频Track、解码器和解码线程。
     */
    bool SubscribeAudioVideo();

    // 设置解码后的视频帧回调(会添加远端用户ID并转发给RenderEngine)
    void SetVideoFrameCallback(OnVideoFrameCallback callback);

    // 设置解码后的音频帧回调。
    void SetAudioFrameCallback(OnAudioFrameCallback callback);

    /**
     * @brief 关闭当前远端用户订阅
     *
     * 关闭顺序：
     * 1. 停止接收新的编码帧。
     * 2. 停止音视频解码线程。
     * 3. 释放解码器。
     * 4. 关闭接收Track。
     * 5. 关闭PeerConnection。
     */
    void Close() override;

private:
    // 通过 VideoCodecFactory 创建H.264解码器
    bool InitializeVideoDecoder();
    // 通过 AudioCodecFactory 创建Opus解码器
    bool InitializeAudioDecoder();
    // 负责创建和配置两个 RecvOnly Track
    bool SetupReceiveTracks();

    /*
     * libdatachannel完成RTP拆包后调用。
     * 这里只执行入队，不直接解码。
     */
    void EnqueueVideoFrame(CODEC::EncodedVideoFrame frame);
    void EnqueueAudioFrame(CODEC::EncodedAudioFrame frame);

    // 取出一个EncodedVideoFrame->必要时发送PLI请求关键帧->调用H.264解码器->调用视频帧回调
    void VideoWorkerLoop();

    void AudioWorkerLoop();

private:
    /*
     * 这些队列用于把解码任务移出libdatachannel回调线程，
     * 不是完整的网络抖动缓冲器。
     */
    static constexpr std::size_t kMaxVideoQueueSize = 6;
    static constexpr std::size_t kMaxAudioQueueSize = 20;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_subscribed{false};

    std::shared_ptr<rtc::Track> m_video_track;
    std::shared_ptr<rtc::Track> m_audio_track;
    mutable std::mutex m_tracks_mutex;

    std::unique_ptr<CODEC::IVideoDecoder> m_video_decoder;
    std::unique_ptr<CODEC::IAudioDecoder> m_audio_decoder;

    std::deque<CODEC::EncodedVideoFrame> m_video_queue;
    std::mutex m_video_queue_mutex;
    std::condition_variable m_video_queue_cv;
    std::thread m_video_worker;

    std::deque<CODEC::EncodedAudioFrame> m_audio_queue;
    std::mutex m_audio_queue_mutex;
    std::condition_variable m_audio_queue_cv;
    std::thread m_audio_worker;

    mutable std::mutex m_callback_mutex;
    // 画面显示 / 扬声器播放
    OnVideoFrameCallback m_video_frame_callback;
    OnAudioFrameCallback m_audio_frame_callback;

     /*
     * 统一保护：
     * - 首包到达时间降级基准；
     * - RTCP SR映射器；
     * - NTP时间线切换状态。
     */
    static constexpr std::uint32_t kVideoRtpClockRate = 90'000;
    static constexpr std::uint32_t kAudioRtpClockRate = 48'000;

    mutable std::mutex m_timebase_mutex;

    bool m_video_timebase_initialized{false};
    std::uint32_t m_video_base_rtp_timestamp{0};
    std::int64_t m_video_base_time_us{0};

    bool m_audio_timebase_initialized{false};
    std::uint32_t m_audio_base_rtp_timestamp{0};
    std::int64_t m_audio_base_time_us{0};

    RtpNtpTimeMapper m_video_time_mapper{
        kVideoRtpClockRate};

    RtpNtpTimeMapper m_audio_time_mapper{
        kAudioRtpClockRate};

    /*
     * true表示音频和视频都已经获得有效RTCP SR，
     * 后续帧统一使用NTP映射时间。
     */
    bool m_rtcp_timebase_active{false};

    /*
     * 将原始NTP时间线平移到当前进程的steady_clock附近。
     *
     * 音频和视频使用完全相同的偏移，因此不会破坏二者
     * 在NTP时间线中的真实相对关系。
     */
    std::int64_t m_ntp_to_local_offset_us{0};
    
    /*
     * 视频编码帧被丢弃后，后续P帧可能无法解码。
     * 队列溢出时由视频解码线程请求新的关键帧。
     */
    bool m_video_queue_overflow{false};

    std::shared_ptr<VideoRtpReorderBuffer> m_video_reorder_buffer;

    // 会话保存
    std::shared_ptr<rtc::RtcpReceivingSession> m_video_rtcp_session;
    std::shared_ptr<rtc::RtcpReceivingSession> m_audio_rtcp_session;

    enum class MediaKind : std::uint8_t {
        kVideo = 0,
        kAudio
    };

    /**
     * @brief 将当前RTP时间戳转换为统一媒体时间
     *
     * RTCP SR尚未同时就绪时使用首包到达时间；
     * 音视频SR都就绪后切换到公共NTP时间线。
     */
    std::int64_t ResolveMediaTimestamp(
        MediaKind media_kind,
        std::uint32_t rtp_timestamp);

    /**
     * @brief 使用首包到达时间建立的降级时间线进行换算
     *
     * 调用时必须持有m_timebase_mutex。
     */
    std::int64_t MapFallbackTimestampLocked(
        MediaKind media_kind,
        std::uint32_t rtp_timestamp) const noexcept;

    /**
     * @brief 时间线切换后清理尚未解码的旧时间线数据
     */
    void ClearPendingFramesForTimebaseSwitch();
};

} // namespace TRANSPORT