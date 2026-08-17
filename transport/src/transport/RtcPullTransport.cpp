#include "RtcPullTransport.h"
#include "VideoRtpReorderBuffer.h"
#include "utils/logManager.h"

#include <rtc/frameinfo.hpp>
#include <rtc/h264rtpdepacketizer.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/rtpdepacketizer.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <utility>
#include <opus/opus.h>

namespace TRANSPORT {

RtcPullTransport::RtcPullTransport() = default;

RtcPullTransport::~RtcPullTransport() {
    Close();
}

bool RtcPullTransport::InitializeVideoDecoder() {
    auto decoder = CODEC::VideoCodecFactory::CreateDecoder(
        CODEC::VideoCodecType::kH264);

    if (!decoder) {
        LOG_ERROR("Failed to create H.264 decoder");
        return false;
    }

    const PublishInfo publish_info = GetPublishInfo();

    CODEC::VideoCodecConfig config;
    config.width = publish_info.video_width;
    config.height = publish_info.video_height;
    config.framerate = publish_info.video_fps;

    /*
     * 解码器最终输出尺寸由H.264码流中的SPS决定。
     * 这里的宽高只用于初始化，不强制远端视频始终保持该尺寸。
     */
    if (!decoder->Initialize(config)) {
        LOG_ERROR("Failed to initialize H.264 decoder");
        return false;
    }

    m_video_decoder = std::move(decoder);

    LOG_INFO("H.264 decoder initialized");
    return true;
}

bool RtcPullTransport::InitializeAudioDecoder() {
    auto decoder = CODEC::AudioCodecFactory::CreateDecoder(
        CODEC::AudioCodecType::kOpus);

    if (!decoder) {
        LOG_ERROR("Failed to create Opus decoder");
        return false;
    }

    const PublishInfo publish_info = GetPublishInfo();

    CODEC::AudioCodecConfig config;
    config.sample_rate = publish_info.audio_sample_rate;
    config.channels = publish_info.audio_channels;
    config.bitrate_kbps =
        publish_info.audio_channels == 1 ? 32 : 64;
    config.frame_size_ms = 20;
    config.enable_fec = true;
    config.enable_plc = true;

    if (!decoder->Initialize(config)) {
        LOG_ERROR("Failed to initialize Opus decoder");
        return false;
    }

    m_audio_decoder = std::move(decoder);

    LOG_INFO("Opus decoder initialized: {}Hz/{}ch",
             config.sample_rate, config.channels);

    return true;
}

bool RtcPullTransport::SubscribeAudioVideo() {
    if (m_subscribed.load(std::memory_order_acquire)) {
        return true;
    }

    if (!Open()) {
        LOG_ERROR("Cannot subscribe: failed to open PeerConnection");
        return false;
    }

    if (!InitializeVideoDecoder() ||
        !InitializeAudioDecoder()) {
        Close();
        return false;
    }

    m_running.store(true, std::memory_order_release);

    try {
        m_video_worker =
            std::thread(&RtcPullTransport::VideoWorkerLoop, this);

        m_audio_worker =
            std::thread(&RtcPullTransport::AudioWorkerLoop, this);
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to start decode workers: {}", exception.what());
        Close();
        return false;
    }

    if (!SetupReceiveTracks()) {
        Close();
        return false;
    }

    m_subscribed.store(true, std::memory_order_release);

    LOG_INFO("Remote audio and video subscription started");
    return true;
}

std::int64_t RtcPullTransport::ResolveMediaTimestamp(
    MediaKind media_kind,
    std::uint32_t rtp_timestamp)
{
    /*
     * 先复制shared_ptr，避免查询SR期间Track被Close()释放。
     */
    std::shared_ptr<rtc::RtcpReceivingSession> video_session;
    std::shared_ptr<rtc::RtcpReceivingSession> audio_session;

    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        video_session = m_video_rtcp_session;
        audio_session = m_audio_rtcp_session;
    }

    rtc::RtcpReceivingSession::SyncTimestamps video_sync{};
    rtc::RtcpReceivingSession::SyncTimestamps audio_sync{};

    if (video_session) {
        video_sync = video_session->getSyncTimestamps();
    }

    if (audio_session) {
        audio_sync = audio_session->getSyncTimestamps();
    }

    const std::int64_t current_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()
                .time_since_epoch())
            .count();

    bool timebase_changed = false;
    std::int64_t result_timestamp_us = 0;
    bool rtcp_active = false;
    {
        std::lock_guard<std::mutex> lock(m_timebase_mutex);

        /*
         * 首次收到该媒体的数据时，先建立降级时间线。
         * 即使暂时没有RTCP SR，媒体也能继续正常播放。
         */
        if (media_kind == MediaKind::kVideo &&
            !m_video_timebase_initialized) {
            m_video_base_rtp_timestamp = rtp_timestamp;
            m_video_base_time_us = current_time_us;
            m_video_timebase_initialized = true;
        }

        if (media_kind == MediaKind::kAudio &&
            !m_audio_timebase_initialized) {
            m_audio_base_rtp_timestamp = rtp_timestamp;
            m_audio_base_time_us = current_time_us;
            m_audio_timebase_initialized = true;
        }

        /*
         * NTP时间戳为0表示RtcpReceivingSession尚未收到有效SR。
         * RTP时间戳本身允许为0，因此不能用RTP字段判断SR有效性。
         */
        if (video_sync.ntpTimestamp != 0) {
            auto update_result =
                m_video_time_mapper.UpdateSenderReport(
                    video_sync.rtpTimestamp,
                    video_sync.ntpTimestamp);

            if (update_result ==
                RtpNtpTimeMapper::UpdateResult::kDiscontinuity) {
                /*
                 * 视频发送端时间线发生跳变。
                 * 接受新的SR并重新建立公共时间线。
                 */
                m_video_time_mapper.Reset();

                m_video_time_mapper.UpdateSenderReport(
                    video_sync.rtpTimestamp,
                    video_sync.ntpTimestamp);

                m_rtcp_timebase_active = false;
                timebase_changed = true;
            }
        }

        if (audio_sync.ntpTimestamp != 0) {
            auto update_result =
                m_audio_time_mapper.UpdateSenderReport(
                    audio_sync.rtpTimestamp,
                    audio_sync.ntpTimestamp);

            if (update_result ==
                RtpNtpTimeMapper::UpdateResult::kDiscontinuity) {
                m_audio_time_mapper.Reset();

                m_audio_time_mapper.UpdateSenderReport(
                    audio_sync.rtpTimestamp,
                    audio_sync.ntpTimestamp);

                m_rtcp_timebase_active = false;
                timebase_changed = true;
            }
        }

        /*
         * 必须等待以下条件同时成立：
         *
         * 1. 音频SR有效；
         * 2. 视频SR有效；
         * 3. 音频降级时间线已建立；
         * 4. 视频降级时间线已建立。
         *
         * 这样可以防止音频使用NTP，而视频仍使用本地到达时间。
         */
        if (!m_rtcp_timebase_active &&
            m_video_time_mapper.IsReady() &&
            m_audio_time_mapper.IsReady() &&
            m_video_timebase_initialized &&
            m_audio_timebase_initialized &&
            audio_sync.ntpTimestamp != 0) {
            /*
             * 使用音频作为连续性基准。
             *
             * 计算同一个音频SR点在：
             * - 原降级时间线；
             * - NTP时间线；
             *
             * 中的差值，并作为公共平移量。
             */
            const auto audio_ntp_time_us =
                m_audio_time_mapper.MapToNtpTimeUs(
                    static_cast<std::uint32_t>(
                        audio_sync.rtpTimestamp));

            if (audio_ntp_time_us) {
                const std::int64_t audio_fallback_time_us =
                    MapFallbackTimestampLocked(
                        MediaKind::kAudio,
                        static_cast<std::uint32_t>(
                            audio_sync.rtpTimestamp));

                m_ntp_to_local_offset_us =
                    audio_fallback_time_us -
                    *audio_ntp_time_us;

                m_rtcp_timebase_active = true;
                timebase_changed = true;
            }
        }

        if (m_rtcp_timebase_active) {
            const auto mapped_time_us =
                media_kind == MediaKind::kVideo
                    ? m_video_time_mapper.MapToNtpTimeUs(
                          rtp_timestamp)
                    : m_audio_time_mapper.MapToNtpTimeUs(
                          rtp_timestamp);

            if (mapped_time_us) {
                result_timestamp_us =
                    *mapped_time_us +
                    m_ntp_to_local_offset_us;
            } else {
                result_timestamp_us =
                    MapFallbackTimestampLocked(
                        media_kind,
                        rtp_timestamp);
            }
        } else {
            result_timestamp_us =
                MapFallbackTimestampLocked(
                    media_kind,
                    rtp_timestamp);
        }
        rtcp_active = m_rtcp_timebase_active;
    }

    if (timebase_changed) {
        /*
         * 清除旧时间线下尚未解码的数据，避免旧时间戳和新时间戳
         * 同时进入后续渲染与混音队列。
         */
        ClearPendingFramesForTimebaseSwitch();

        LOG_INFO(
            "RTC media timebase updated: "
            "rtcp_sync_active={}",
            rtcp_active);
    }

    return result_timestamp_us;
}

std::int64_t RtcPullTransport::MapFallbackTimestampLocked(
    MediaKind media_kind,
    std::uint32_t rtp_timestamp) const noexcept
{
    if (media_kind == MediaKind::kVideo) {
        const std::int32_t rtp_delta =
            static_cast<std::int32_t>(
                rtp_timestamp -
                m_video_base_rtp_timestamp);

        return m_video_base_time_us +
               static_cast<std::int64_t>(rtp_delta) *
                   1'000'000LL /
                   kVideoRtpClockRate;
    }

    const std::int32_t rtp_delta =
        static_cast<std::int32_t>(
            rtp_timestamp -
            m_audio_base_rtp_timestamp);

    return m_audio_base_time_us +
           static_cast<std::int64_t>(rtp_delta) *
               1'000'000LL /
               kAudioRtpClockRate;
}

void RtcPullTransport::
ClearPendingFramesForTimebaseSwitch()
{
    std::scoped_lock lock(
        m_video_queue_mutex,
        m_audio_queue_mutex);

    m_video_queue.clear();
    m_audio_queue.clear();

    /*
     * 清理视频编码队列可能破坏H.264参考帧连续性，
     * 通知视频工作线程请求新的IDR关键帧。
     */
    m_video_queue_overflow = true;
}

bool RtcPullTransport::SetupReceiveTracks() {
    std::shared_ptr<rtc::PeerConnection> connection;

    const PublishInfo publish_info = GetPublishInfo();
    const int video_width = publish_info.video_width;
    const int video_height = publish_info.video_height;

    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);

        if (m_video_track && m_audio_track) {
            return true;
        }

        connection = m_peer_connection;
        if (!connection) {
            LOG_ERROR("Cannot create receive tracks: PeerConnection is null");
            return false;
        }

        try {
            rtc::Description::Video video_description(
                "video",
                rtc::Description::Direction::RecvOnly);

            video_description.addH264Codec(
                kH264PayloadType,
                "profile-level-id=42e01f;"
                "packetization-mode=1;"
                "level-asymmetry-allowed=1");

            rtc::Description::Audio audio_description(
                "audio",
                rtc::Description::Direction::RecvOnly);

            std::string opus_profile =
                "minptime=10;useinbandfec=1";

            if (GetPublishInfo().audio_channels == 2) {
                opus_profile +=
                    ";stereo=1;sprop-stereo=1";
            }

            audio_description.addOpusCodec(
                kOpusPayloadType,
                opus_profile);

            m_video_track =
                connection->addTrack(video_description);

            m_audio_track =
                connection->addTrack(audio_description);

            if (!m_video_track || !m_audio_track) {
                LOG_ERROR("Failed to create RTC receive tracks");
                m_video_track.reset();
                m_audio_track.reset();
                return false;
            }

            /*
             * RtcpReceivingSession处理接收端RTCP信息并支持PLI请求。
             * H264RtpDepacketizer将多个RTP包重新组合为完整Annex-B帧。
             */
            auto video_depacketizer =
                std::make_shared<rtc::H264RtpDepacketizer>(
                    rtc::H264RtpDepacketizer::Separator::
                        LongStartSequence);

            m_video_rtcp_session =
                std::make_shared<rtc::RtcpReceivingSession>();

            // vedio处理           
            std::weak_ptr<rtc::Track> weak_video_track =
                m_video_track;

            auto video_reorder_buffer =
                std::make_shared<VideoRtpReorderBuffer>(
                    std::chrono::milliseconds(50),
                    6,
                    [weak_video_track]() {
                        auto track = weak_video_track.lock();

                        if (!track || !track->isOpen()) {
                            return;
                        }

                        try {
                            track->requestKeyframe();
                        } catch (...) {
                            // 不允许异常进入libdatachannel线程。
                        }
                    });

            m_video_reorder_buffer = video_reorder_buffer;

            /*
            * incomingChain按链尾到链头执行，因此实际接收顺序为：
            * RtcpReceivingSession
            *   -> VideoRtpReorderBuffer
            *   -> H264RtpDepacketizer
            */
            m_video_track->setMediaHandler(video_depacketizer);
            m_video_track->chainMediaHandler(video_reorder_buffer);
            m_video_track->chainMediaHandler(m_video_rtcp_session);



            // audio处理           
            m_audio_rtcp_session =
                std::make_shared<rtc::RtcpReceivingSession>();

            auto audio_depacketizer =
                std::make_shared<rtc::OpusRtpDepacketizer>();

            m_audio_track->setMediaHandler(audio_depacketizer);
            m_audio_track->chainMediaHandler(m_audio_rtcp_session);

            /*
             * Track完成RTP拆包后产生一帧完整H.264数据。
             * 回调中只复制编码数据、转换时间戳并入队。
             */
            m_video_track->onFrame(
                [this, video_width, video_height](rtc::binary data, rtc::FrameInfo info) {
                    if (!m_running.load(std::memory_order_acquire) ||
                        data.empty()) {
                        return;
                    }

                    CODEC::EncodedVideoFrame frame;
                    frame.data.resize(data.size());

                    std::memcpy(frame.data.data(), data.data(), data.size());

                    frame.width = video_width;
                    frame.height = video_height;
                    frame.codec_type = CODEC::VideoCodecType::kH264;
                    frame.is_skipped = false;

                    frame.timestamp_us = ResolveMediaTimestamp(MediaKind::kVideo,info.timestamp);

                    EnqueueVideoFrame(std::move(frame));
                });

            m_audio_track->onFrame(
                [this](rtc::binary data, rtc::FrameInfo info) {
                    if (!m_running.load(std::memory_order_acquire) ||
                        data.empty()) {
                        return;
                    }

                    const int samples_per_channel = opus_packet_get_nb_samples(
                        reinterpret_cast<const unsigned char*>(data.data()),
                        static_cast<opus_int32>(data.size()),
                        48000);

                    if (samples_per_channel <= 0) {
                        LOG_ERROR("Invalid Opus packet: {}",
                                opus_strerror(samples_per_channel));
                        return;
                    }


                    CODEC::EncodedAudioFrame frame;
                    frame.data.resize(data.size());
                    frame.samples_per_channel = static_cast<std::uint32_t>(samples_per_channel);
                    frame.codec_type = CODEC::AudioCodecType::kOpus;

                    std::memcpy(frame.data.data(), data.data(), data.size());

                    frame.timestamp_us = ResolveMediaTimestamp(MediaKind::kAudio, info.timestamp);

                    EnqueueAudioFrame(std::move(frame));
                });

            m_video_track->onOpen(
                [this]() {
                    std::shared_ptr<rtc::Track> track;

                    {
                        std::lock_guard<std::mutex> lock(
                            m_tracks_mutex);
                        track = m_video_track;
                    }

                    LOG_INFO("Remote video track opened");

                    /*
                     * 新订阅者通常无法从中间的P帧开始解码，
                     * 因此Track打开后立即请求一个关键帧。
                     */
                    if (track) {
                        track->requestKeyframe();
                    }
                });

            m_audio_track->onOpen(
                []() {
                    LOG_INFO("Remote audio track opened");
                });

            m_video_track->onClosed(
                []() {
                    LOG_INFO("Remote video track closed");
                });

            m_audio_track->onClosed(
                []() {
                    LOG_INFO("Remote audio track closed");
                });

            m_video_track->onError(
                [](const std::string& error) {
                    LOG_ERROR("Remote video track error: {}", error);
                });

            m_audio_track->onError(
                [](const std::string& error) {
                    LOG_ERROR("Remote audio track error: {}", error);
                });
        } catch (const std::exception& exception) {
            LOG_ERROR("Failed to configure receive tracks: {}",
                      exception.what());

            if (m_video_track) {
                m_video_track->resetCallbacks();
                m_video_track->close();
                m_video_track.reset();
            }

            if (m_audio_track) {
                m_audio_track->resetCallbacks();
                m_audio_track->close();
                m_audio_track.reset();
            }

            return false;
        }
    }

    try {
        /*
         * 音视频RecvOnly Track全部创建后再生成WHEP Offer。
         */
        connection->setLocalDescription(
            rtc::Description::Type::Offer);
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to create WHEP Offer SDP: {}",
                  exception.what());

        std::lock_guard<std::mutex> lock(m_tracks_mutex);

        if (m_video_track) {
            m_video_track->resetCallbacks();
            m_video_track->close();
            m_video_track.reset();
        }

        if (m_audio_track) {
            m_audio_track->resetCallbacks();
            m_audio_track->close();
            m_audio_track.reset();
        }

        return false;
    }

    LOG_INFO("RTC audio and video receive tracks created");
    return true;
}

void RtcPullTransport::SetVideoFrameCallback(
    OnVideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_video_frame_callback = std::move(callback);
}

void RtcPullTransport::SetAudioFrameCallback(
    OnAudioFrameCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_audio_frame_callback = std::move(callback);
}

void RtcPullTransport::EnqueueVideoFrame(
    CODEC::EncodedVideoFrame frame) {
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_video_queue_mutex);

        if (!m_running.load(std::memory_order_relaxed)) {
            return;
        }

        if (m_video_queue.size() >= kMaxVideoQueueSize) {
            m_video_queue.pop_front();
            m_video_queue_overflow = true;
            ++m_video_queue_drop_count;
        }

        m_video_queue.push_back(std::move(frame));
    }

    m_video_queue_cv.notify_one();
}

void RtcPullTransport::EnqueueAudioFrame(
    CODEC::EncodedAudioFrame frame) {
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);

        if (!m_running.load(std::memory_order_relaxed)) {
            return;
        }

        if (m_audio_queue.size() >= kMaxAudioQueueSize) {
            m_audio_queue.pop_front();
        }

        m_audio_queue.push_back(std::move(frame));
    }

    m_audio_queue_cv.notify_one();
}

void RtcPullTransport::VideoWorkerLoop() {
    auto request_keyframe =
        [this]() {
            std::shared_ptr<rtc::Track> track;

            {
                std::lock_guard<std::mutex> lock(m_tracks_mutex);
                track = m_video_track;
            }

            if (!track || !track->isOpen()) {
                return false;
            }

            try {
                return track->requestKeyframe();
            } catch (const std::exception& exception) {
                LOG_ERROR("Failed to request video keyframe: {}",
                          exception.what());
                return false;
            }
        };

    while (true) {
        CODEC::EncodedVideoFrame encoded_frame;
        bool queue_overflow = false;

        {
            std::unique_lock<std::mutex> lock(m_video_queue_mutex);

            m_video_queue_cv.wait(
                lock,
                [this]() {
                    return !m_running.load(
                               std::memory_order_acquire) ||
                           !m_video_queue.empty();
                });

            if (m_video_queue.empty()) {
                if (!m_running.load(
                        std::memory_order_acquire)) {
                    return;
                }

                continue;
            }

            encoded_frame =
                std::move(m_video_queue.front());

            m_video_queue.pop_front();

            queue_overflow = m_video_queue_overflow;
            m_video_queue_overflow = false;
        }

        /*
         * 队列溢出意味着至少丢失了一帧H.264数据。
         * 后续P帧可能依赖被丢弃的帧，因此主动请求新关键帧。
         */
        if (queue_overflow) {
            request_keyframe();
        }

        if (!m_video_decoder) {
            continue;
        }

        std::shared_ptr<CODEC::RawVideoFrame> decoded_frame;

        if (!m_video_decoder->Decode(
                encoded_frame,
                decoded_frame)) {
            if (m_video_decoder->NeedsKeyframe() &&
                request_keyframe()) {
                m_video_decoder->ClearKeyframeRequest();
            }

            continue;
        }

        if (!decoded_frame ||
            !decoded_frame->IsValid()) {
            continue;
        }

        OnVideoFrameCallback callback;

        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            callback = m_video_frame_callback;
        }

        if (!callback) {
            continue;
        }

        try {
            callback(decoded_frame);
        } catch (const std::exception& exception) {
            LOG_ERROR("Video frame callback failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR("Video frame callback failed: unknown exception");
        }
    }
}

void RtcPullTransport::AudioWorkerLoop() {
    while (true) {
        CODEC::EncodedAudioFrame encoded_frame;

        {
            std::unique_lock<std::mutex> lock(m_audio_queue_mutex);

            m_audio_queue_cv.wait(
                lock,
                [this]() {
                    return !m_running.load(
                               std::memory_order_acquire) ||
                           !m_audio_queue.empty();
                });

            if (m_audio_queue.empty()) {
                if (!m_running.load(
                        std::memory_order_acquire)) {
                    return;
                }

                continue;
            }

            encoded_frame =
                std::move(m_audio_queue.front());

            m_audio_queue.pop_front();
        }

        if (!m_audio_decoder ||
            !m_audio_decoder->PushInput(encoded_frame)) {
            continue;
        }

        while (true) {
            std::shared_ptr<CODEC::RawAudioFrame> decoded_frame;

            if (!m_audio_decoder->PullDecoded(decoded_frame)) {
                break;
            }

            if (!decoded_frame ||
                !decoded_frame->IsValid()) {
                continue;
            }

            OnAudioFrameCallback callback;

            {
                std::lock_guard<std::mutex> lock(m_callback_mutex);
                callback = m_audio_frame_callback;
            }

            if (!callback) {
                continue;
            }

            try {
                callback(decoded_frame);
            } catch (const std::exception& exception) {
                LOG_ERROR("Audio frame callback failed: {}",
                          exception.what());
            } catch (...) {
                LOG_ERROR("Audio frame callback failed: unknown exception");
            }
        }
    }
}

void RtcPullTransport::Close() {
    m_subscribed.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);

    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::shared_ptr<VideoRtpReorderBuffer> video_reorder_buffer;
    
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        video_track = std::move(m_video_track);
        audio_track = std::move(m_audio_track);
        video_reorder_buffer = std::move(m_video_reorder_buffer);

        m_video_rtcp_session.reset();
        m_audio_rtcp_session.reset();
    }

    /*
     * 先解除Track回调并关闭Track，防止继续向解码队列写入数据。
     */
    try {
        if (video_track) {
            video_track->resetCallbacks();
            video_track->close();
        }

        if (audio_track) {
            audio_track->resetCallbacks();
            audio_track->close();
        }
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to close receive tracks: {}",exception.what());
                  
    }

    /*
     * 清空尚未解码的音视频数据。
     *
     * 两个队列一起加锁，避免关闭过程中还有线程分别访问队列。
     */
    {
        std::scoped_lock lock(m_video_queue_mutex,m_audio_queue_mutex);

        m_video_queue.clear();
        m_audio_queue.clear();
        m_video_queue_overflow = false;
    }

    /*
     * 唤醒两个解码线程。
     *
     * 此时m_running已经为false且队列已经清空，
     * 因此工作线程会从等待状态退出。
     */
    m_video_queue_cv.notify_all();
    m_audio_queue_cv.notify_all();

    if (m_video_worker.joinable()) {
        m_video_worker.join();
    }

    if (m_audio_worker.joinable()) {
        m_audio_worker.join();
    }

    /*
     * 解码线程完全结束后再释放解码器。
     */
    m_video_decoder.reset();
    m_audio_decoder.reset();

    /*
     * 统一重置降级时间线和RTCP SR映射状态。
     *
     * 这些成员不再由音视频队列锁保护，
     * 必须使用独立的m_timebase_mutex。
     */
    {
        std::lock_guard<std::mutex> lock(
            m_timebase_mutex);

        m_video_time_mapper.Reset();
        m_audio_time_mapper.Reset();

        m_rtcp_timebase_active = false;
        m_ntp_to_local_offset_us = 0;

        m_video_timebase_initialized = false;
        m_video_base_rtp_timestamp = 0;
        m_video_base_time_us = 0;

        m_audio_timebase_initialized = false;
        m_audio_base_rtp_timestamp = 0;
        m_audio_base_time_us = 0;
    }

    /*
     * 清理上层解码帧回调，避免关闭后继续持有外部对象。
     */
    {
        std::lock_guard<std::mutex> lock(
            m_callback_mutex);

        m_video_frame_callback = nullptr;
        m_audio_frame_callback = nullptr;
    }

    ClosePeerConnection();

    /*
     * 输出本次会话的RTP乱序处理统计。
     */
    if (video_reorder_buffer) {
        const auto statistics =
            video_reorder_buffer->GetStatistics();

        LOG_INFO(
            "Video RTP reorder statistics: "
            "received={}, released={}, reordered={}, "
            "duplicate={}, late={}, invalid={}, "
            "released_frames={}, dropped_frames={}, missing={}",
            statistics.received_packets,
            statistics.released_packets,
            statistics.reordered_packets,
            statistics.duplicate_packets,
            statistics.late_packets,
            statistics.invalid_packets,
            statistics.released_frames,
            statistics.dropped_frames,
            statistics.missing_packets);

        video_reorder_buffer->Reset();
    }
}

} // namespace TRANSPORT