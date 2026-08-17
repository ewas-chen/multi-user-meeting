#include "RtcPushTransport.h"

#include "utils/logManager.h"

#include <rtc/frameinfo.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizer.hpp>
#include <rtc/rtppacketizationconfig.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <random>
#include <utility>

namespace {

/**
 * @brief 生成非零SSRC
 *
 * SSRC用于在RTP会话中标识一路媒体。
 * 音频和视频必须使用不同的SSRC。
 */
std::uint32_t GenerateSsrc() {
    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_int_distribution<std::uint32_t> distribution(
        1, std::numeric_limits<std::uint32_t>::max());

    return distribution(generator);
}

/**
 * @brief 将采集时间戳转换为相对于当前推流会话的秒数
 *
 * 音频和视频共用session_start_time_us，因此能够保留采集时的
 * 相对时间关系。libdatachannel随后会根据各自RTP时钟频率转换：
 *
 * H.264：90000 Hz
 * Opus：48000 Hz
 */
double GetRelativeTimestampSeconds(
    std::atomic<std::int64_t>& session_start_time_us,
    std::int64_t timestamp_us) noexcept {
    if (timestamp_us <= 0) {
        return 0.0;
    }

    std::int64_t expected = 0;
    session_start_time_us.compare_exchange_strong(
        expected, timestamp_us, std::memory_order_acq_rel);

    const std::int64_t start =
        session_start_time_us.load(std::memory_order_acquire);

    if (timestamp_us <= start) {
        return 0.0;
    }

    return static_cast<double>(timestamp_us - start) / 1000000.0;
}

} // namespace

namespace TRANSPORT {

RtcPushTransport::RtcPushTransport() = default;

RtcPushTransport::~RtcPushTransport() {
    Close();
}

bool RtcPushTransport::StartPublishVideo(const std::string& cname) {
    if (cname.empty()) {
        LOG_ERROR("Cannot start video publishing: CNAME is empty");
        return false;
    }

    if (m_video_publishing.load(std::memory_order_acquire)) {
        return true;
    }

    if (!Open()) {
        LOG_ERROR("Cannot start video publishing: failed to open PeerConnection");
        return false;
    }

    if (!InitializeVideoEncoder()) {
        return false;
    }

    if (!SetupPublishTracks(cname)) {
        std::lock_guard<std::mutex> lock(m_video_encoder_mutex);
        m_video_encoder.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_video_queue_mutex);
        m_video_queue.clear();
    }

    ResetVideoBitrateFeedback();
    m_video_publishing.store(true, std::memory_order_release);

    try {
        m_video_worker = std::thread(&RtcPushTransport::VideoWorkerLoop, this);
    } catch (const std::exception& exception) {
        m_video_publishing.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lock(m_video_encoder_mutex);
        m_video_encoder.reset();

        LOG_ERROR("Failed to start video worker: {}", exception.what());
        return false;
    }

    LOG_INFO("Video publishing started");
    return true;
}

bool RtcPushTransport::StopPublishVideo() {
    const bool was_publishing =
        m_video_publishing.exchange(false, std::memory_order_acq_rel);

    m_video_queue_cv.notify_all();

    if (m_video_worker.joinable()) {
        m_video_worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_video_queue_mutex);
        m_video_queue.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_video_encoder_mutex);
        m_video_encoder.reset();
    }

    m_video_bitrate_controller.Reset();
    ResetVideoBitrateFeedback();

    if (was_publishing) {
        LOG_INFO("Video publishing stopped");
    }

    return true;
}

bool RtcPushTransport::StartPublishAudio(const std::string& cname) {
    if (cname.empty()) {
        LOG_ERROR("Cannot start audio publishing: CNAME is empty");
        return false;
    }

    if (m_audio_publishing.load(std::memory_order_acquire)) {
        return true;
    }

    if (!Open()) {
        LOG_ERROR("Cannot start audio publishing: failed to open PeerConnection");
        return false;
    }

    if (!InitializeAudioEncoder()) {
        return false;
    }

    if (!SetupPublishTracks(cname)) {
        std::lock_guard<std::mutex> lock(m_audio_encoder_mutex);
        m_audio_encoder.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        m_audio_queue.clear();
    }

    m_audio_publishing.store(true, std::memory_order_release);

    try {
        m_audio_worker = std::thread(&RtcPushTransport::AudioWorkerLoop, this);
    } catch (const std::exception& exception) {
        m_audio_publishing.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lock(m_audio_encoder_mutex);
        m_audio_encoder.reset();

        LOG_ERROR("Failed to start audio worker: {}", exception.what());
        return false;
    }

    LOG_INFO("Audio publishing started");
    return true;
}

bool RtcPushTransport::StopPublishAudio() {
    const bool was_publishing =
        m_audio_publishing.exchange(false, std::memory_order_acq_rel);

    m_audio_queue_cv.notify_all();

    if (m_audio_worker.joinable()) {
        m_audio_worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        m_audio_queue.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_audio_encoder_mutex);
        m_audio_encoder.reset();
    }

    if (was_publishing) {
        LOG_INFO("Audio publishing stopped");
    }

    return true;
}

bool RtcPushTransport::PushVideoFrame(const std::shared_ptr<I420Frame>& frame) {
    if (!m_video_publishing.load(std::memory_order_acquire) ||
        !frame || !frame->IsValid()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_video_queue_mutex);

        /*
         * 编码速度跟不上采集速度时丢弃最旧帧。
         * 丢帧次数作为码率控制器的发送端压力信号。
         */
        if (m_video_queue.size() >= kMaxVideoQueueSize) {
            m_video_queue.pop_front();
            m_video_queue_drop_count.fetch_add(1, std::memory_order_relaxed);
        }

        m_video_queue.push_back(frame);
    }

    m_video_queue_cv.notify_one();
    return true;
}

bool RtcPushTransport::PushAudioFrame(const std::shared_ptr<AudioFrame>& frame) {
    if (!m_audio_publishing.load(std::memory_order_acquire) ||
        !frame || !frame->IsValid()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);

        /*
         * 音频积压同样会导致会议延迟不断增加。
         * 队列达到上限时丢弃最旧数据，保持实时性。
         */
        if (m_audio_queue.size() >= kMaxAudioQueueSize) {
            m_audio_queue.pop_front();
        }

        m_audio_queue.push_back(frame);
    }

    m_audio_queue_cv.notify_one();
    return true;
}

bool RtcPushTransport::IsPublishingVideo() const noexcept {
    return m_video_publishing.load(std::memory_order_acquire);
}

bool RtcPushTransport::IsPublishingAudio() const noexcept {
    return m_audio_publishing.load(std::memory_order_acquire);
}

bool RtcPushTransport::InitializeVideoEncoder() {
    const PublishInfo publish_info = GetPublishInfo();

    auto encoder =
        CODEC::VideoCodecFactory::CreateEncoder(CODEC::VideoCodecType::kH264);

    if (!encoder) {
        LOG_ERROR("Failed to create H.264 encoder");
        return false;
    }

    /*
     * 根据分辨率和帧率估算基础目标码率。
     * 自适应控制器只在压力下降低，并在稳定后恢复到该基础值。
     */
    const std::int64_t estimated_bitrate =
        static_cast<std::int64_t>(publish_info.video_width) *
        publish_info.video_height * publish_info.video_fps / 8000;

    const std::uint32_t target_bitrate = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(estimated_bitrate, 300, 6000));

    CODEC::VideoCodecConfig config;
    config.width = publish_info.video_width;
    config.height = publish_info.video_height;
    config.framerate = publish_info.video_fps;
    config.target_bitrate_kbps = target_bitrate;
    config.min_bitrate_kbps =
        std::max<std::uint32_t>(200, target_bitrate / 2);
    config.max_bitrate_kbps = target_bitrate * 3 / 2;
    config.keyframe_interval =
        static_cast<std::uint32_t>(publish_info.video_fps * 2);
    config.threads = 0;
    config.enable_frame_dropping = true;

    if (!encoder->Initialize(config)) {
        LOG_ERROR("Failed to initialize H.264 encoder");
        return false;
    }

    AdaptiveVideoBitrateConfig bitrate_config;
    bitrate_config.min_bitrate_kbps = config.min_bitrate_kbps;
    bitrate_config.initial_bitrate_kbps = config.target_bitrate_kbps;

    /*
     * 保守版本不主动超过初始目标码率。
     * OpenH264仍保留更高的max_bitrate_kbps供后续完整拥塞控制使用。
     */
    bitrate_config.max_bitrate_kbps = config.target_bitrate_kbps;
    bitrate_config.congested_queue_depth = kMaxVideoQueueSize - 1;
    bitrate_config.stable_queue_depth = 1;

    if (!m_video_bitrate_controller.Initialize(bitrate_config)) {
        LOG_ERROR("Failed to initialize adaptive video bitrate controller");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_video_encoder_mutex);
        m_video_encoder = std::move(encoder);
    }

    LOG_INFO(
        "H.264 encoder initialized: {}x{}@{}fps, bitrate={}kbps, adaptive_range={}-{}kbps",
        config.width, config.height, config.framerate,
        config.target_bitrate_kbps, bitrate_config.min_bitrate_kbps,
        bitrate_config.max_bitrate_kbps);

    return true;
}

bool RtcPushTransport::InitializeAudioEncoder() {
    const PublishInfo publish_info = GetPublishInfo();

    auto encoder =
        CODEC::AudioCodecFactory::CreateEncoder(CODEC::AudioCodecType::kOpus);

    if (!encoder) {
        LOG_ERROR("Failed to create Opus encoder");
        return false;
    }

    CODEC::AudioCodecConfig config;
    config.sample_rate = publish_info.audio_sample_rate;
    config.channels = publish_info.audio_channels;
    config.bitrate_kbps = publish_info.audio_channels == 1 ? 32 : 64;
    config.frame_size_ms = 20;
    config.complexity = 5;
    config.enable_vbr = true;
    config.enable_dtx = false;
    config.enable_fec = true;
    config.enable_plc = true;

    if (!encoder->Initialize(config)) {
        LOG_ERROR("Failed to initialize Opus encoder");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_audio_encoder_mutex);
        m_audio_encoder = std::move(encoder);
    }

    LOG_INFO("Opus encoder initialized: {}Hz/{}ch, bitrate={}kbps",
             config.sample_rate, config.channels, config.bitrate_kbps);

    return true;
}

void RtcPushTransport::ResetVideoBitrateFeedback() noexcept {
    m_video_queue_drop_count.store(0, std::memory_order_relaxed);
    m_video_send_failure_count.store(0, std::memory_order_relaxed);
}

void RtcPushTransport::EvaluateVideoBitrate(
    std::uint32_t sample_duration_ms,
    std::size_t queue_depth) {
    VideoBitrateFeedback feedback;
    feedback.sample_duration_ms = sample_duration_ms;
    feedback.queue_depth = queue_depth;
    feedback.queue_drop_count =
        m_video_queue_drop_count.exchange(0, std::memory_order_relaxed);
    feedback.send_failure_count =
        m_video_send_failure_count.exchange(0, std::memory_order_relaxed);

    const std::uint32_t previous_bitrate =
        m_video_bitrate_controller.GetCurrentBitrateKbps();

    const std::optional<std::uint32_t> new_bitrate =
        m_video_bitrate_controller.Update(feedback);

    if (!new_bitrate) {
        return;
    }

    bool applied = false;

    {
        std::lock_guard<std::mutex> lock(m_video_encoder_mutex);
        applied = m_video_encoder && m_video_encoder->SetBitrate(*new_bitrate);
    }

    if (applied) {
        LOG_INFO(
            "Adaptive video bitrate changed: {} -> {}kbps, queue={}, dropped={}, send_failures={}",
            previous_bitrate, *new_bitrate, feedback.queue_depth,
            feedback.queue_drop_count, feedback.send_failure_count);
        return;
    }

    /*
     * SetBitrate失败时编码器仍保持之前的码率。
     * 将控制器锁定在该码率，避免控制状态与编码器状态继续偏离。
     * 下一次重新发布视频时会重新启用自适应控制。
     */
    AdaptiveVideoBitrateConfig fallback_config;
    fallback_config.min_bitrate_kbps = previous_bitrate;
    fallback_config.initial_bitrate_kbps = previous_bitrate;
    fallback_config.max_bitrate_kbps = previous_bitrate;
    m_video_bitrate_controller.Initialize(fallback_config);

    LOG_WARN(
        "Failed to apply adaptive video bitrate {}kbps; adaptation disabled at {}kbps for current session",
        *new_bitrate, previous_bitrate);
}

bool RtcPushTransport::SetupPublishTracks(const std::string& cname) {
    std::shared_ptr<rtc::PeerConnection> connection;

    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);

        if (m_tracks_initialized) {
            return true;
        }

        connection = m_peer_connection;
        if (!connection) {
            LOG_ERROR("Cannot create publish tracks: PeerConnection is null");
            return false;
        }

        try {
            const std::uint32_t video_ssrc = GenerateSsrc();
            const std::uint32_t audio_ssrc = GenerateSsrc();
            const std::string media_stream_id = cname + "-stream";

            /*
             * H.264使用Baseline Profile和packetization-mode=1，
             * 与OpenH264当前配置和FU-A分片方式保持一致。
             */
            rtc::Description::Video video_description(
                "video", rtc::Description::Direction::SendOnly);

            video_description.addH264Codec(
                kH264PayloadType,
                "profile-level-id=42e01f;"
                "packetization-mode=1;"
                "level-asymmetry-allowed=1");

            video_description.addSSRC(
                video_ssrc, cname, media_stream_id, cname + "-video");

            rtc::Description::Audio audio_description(
                "audio", rtc::Description::Direction::SendOnly);

            std::string opus_profile = "minptime=10;useinbandfec=1";
            if (GetPublishInfo().audio_channels == 2) {
                opus_profile += ";stereo=1;sprop-stereo=1";
            }

            audio_description.addOpusCodec(kOpusPayloadType, opus_profile);
            audio_description.addSSRC(
                audio_ssrc, cname, media_stream_id, cname + "-audio");

            m_video_track = connection->addTrack(video_description);
            m_audio_track = connection->addTrack(audio_description);

            if (!m_video_track || !m_audio_track) {
                LOG_ERROR("Failed to create RTC publish tracks");
                m_video_track.reset();
                m_audio_track.reset();
                return false;
            }

            auto video_rtp_config =
                std::make_shared<rtc::RtpPacketizationConfig>(
                    video_ssrc, cname, kH264PayloadType,
                    rtc::H264RtpPacketizer::ClockRate);

            auto video_packetizer =
                std::make_shared<rtc::H264RtpPacketizer>(
                    rtc::H264RtpPacketizer::Separator::LongStartSequence,
                    video_rtp_config);

            /*
             * SR：发送RTP时间戳与NTP时间的对应关系。
             * NACK：保存近期RTP包，用于接收端请求重传。
             * PLI：接收端无法解码时请求新的H.264关键帧。
             */
            video_packetizer->addToChain(
                std::make_shared<rtc::RtcpSrReporter>(video_rtp_config));

            video_packetizer->addToChain(
                std::make_shared<rtc::RtcpNackResponder>());

            video_packetizer->addToChain(
                std::make_shared<rtc::PliHandler>(
                    [this]() {
                        std::lock_guard<std::mutex> lock(
                            m_video_encoder_mutex);

                        if (m_video_encoder) {
                            m_video_encoder->RequestKeyframe();
                        }
                    }));

            m_video_track->setMediaHandler(video_packetizer);

            auto audio_rtp_config =
                std::make_shared<rtc::RtpPacketizationConfig>(
                    audio_ssrc, cname, kOpusPayloadType,
                    rtc::OpusRtpPacketizer::DefaultClockRate);

            auto audio_packetizer =
                std::make_shared<rtc::OpusRtpPacketizer>(audio_rtp_config);

            audio_packetizer->addToChain(
                std::make_shared<rtc::RtcpSrReporter>(audio_rtp_config));

            audio_packetizer->addToChain(
                std::make_shared<rtc::RtcpNackResponder>());

            m_audio_track->setMediaHandler(audio_packetizer);
            m_tracks_initialized = true;
        } catch (const std::exception& exception) {
            LOG_ERROR("Failed to configure publish tracks: {}",
                      exception.what());

            if (m_video_track) {
                m_video_track->close();
                m_video_track.reset();
            }

            if (m_audio_track) {
                m_audio_track->close();
                m_audio_track.reset();
            }

            m_tracks_initialized = false;
            return false;
        }
    }

    try {
        /*
         * Track全部添加完成后再生成Offer，
         * 保证SDP中同时存在音频和视频媒体描述。
         */
        connection->setLocalDescription(rtc::Description::Type::Offer);
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to create local Offer SDP: {}", exception.what());

        std::lock_guard<std::mutex> lock(m_tracks_mutex);

        if (m_video_track) {
            m_video_track->close();
            m_video_track.reset();
        }

        if (m_audio_track) {
            m_audio_track->close();
            m_audio_track.reset();
        }

        m_tracks_initialized = false;
        return false;
    }

    LOG_INFO("RTC audio and video publish tracks created");
    return true;
}

void RtcPushTransport::VideoWorkerLoop() {
    auto last_bitrate_evaluation_time = std::chrono::steady_clock::now();

    while (true) {
        std::shared_ptr<I420Frame> frame;
        std::size_t queue_depth = 0;

        {
            std::unique_lock<std::mutex> lock(m_video_queue_mutex);

            m_video_queue_cv.wait(
                lock,
                [this]() {
                    return !m_video_publishing.load(
                               std::memory_order_acquire) ||
                           !m_video_queue.empty();
                });

            if (m_video_queue.empty()) {
                if (!m_video_publishing.load(std::memory_order_acquire)) {
                    return;
                }

                continue;
            }

            frame = std::move(m_video_queue.front());
            m_video_queue.pop_front();
            queue_depth = m_video_queue.size();
        }

        /*
         * 每秒提交一次发送端反馈。
         * 评估在视频工作线程中执行，不阻塞OBS采集回调。
         */
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_bitrate_evaluation_time).count();

        if (elapsed_ms >= kVideoBitrateEvaluationIntervalMs) {
            const auto limited_elapsed_ms = std::min<std::int64_t>(
                elapsed_ms,
                static_cast<std::int64_t>(
                    std::numeric_limits<std::uint32_t>::max()));

            EvaluateVideoBitrate(
                static_cast<std::uint32_t>(limited_elapsed_ms),
                queue_depth);

            last_bitrate_evaluation_time = now;
        }

        CODEC::EncodedVideoFrame encoded_frame;

        {
            std::lock_guard<std::mutex> lock(m_video_encoder_mutex);

            if (!m_video_encoder ||
                !m_video_encoder->Encode(frame, encoded_frame)) {
                LOG_ERROR("Failed to encode H.264 frame");
                continue;
            }
        }

        if (encoded_frame.is_skipped || encoded_frame.data.empty()) {
            continue;
        }

        std::shared_ptr<rtc::Track> track;

        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            track = m_video_track;
        }

        if (!track || !track->isOpen()) {
            continue;
        }

        const double relative_seconds =
            GetRelativeTimestampSeconds(
                m_session_start_time_us,
                encoded_frame.timestamp_us);

        rtc::FrameInfo frame_info(0u);
        frame_info.payloadType = kH264PayloadType;
        frame_info.timestampSeconds =
            std::chrono::duration<double>(relative_seconds);

        rtc::binary sample(
            reinterpret_cast<const std::byte*>(encoded_frame.data.data()),
            reinterpret_cast<const std::byte*>(
                encoded_frame.data.data() + encoded_frame.data.size()));

        try {
            track->sendFrame(std::move(sample), frame_info);
        } catch (const std::exception& exception) {
            m_video_send_failure_count.fetch_add(
                1, std::memory_order_relaxed);

            LOG_ERROR("Failed to send H.264 frame: {}", exception.what());
        }
    }
}

void RtcPushTransport::AudioWorkerLoop() {
    while (true) {
        std::shared_ptr<AudioFrame> frame;
        bool should_stop = false;

        {
            std::unique_lock<std::mutex> lock(m_audio_queue_mutex);

            m_audio_queue_cv.wait(
                lock,
                [this]() {
                    return !m_audio_publishing.load(
                               std::memory_order_acquire) ||
                           !m_audio_queue.empty();
                });

            if (!m_audio_queue.empty()) {
                frame = std::move(m_audio_queue.front());
                m_audio_queue.pop_front();
            } else if (!m_audio_publishing.load(
                           std::memory_order_acquire)) {
                should_stop = true;
            }
        }

        if (frame) {
            std::lock_guard<std::mutex> lock(m_audio_encoder_mutex);

            if (!m_audio_encoder || !m_audio_encoder->PushInput(frame)) {
                LOG_ERROR("Failed to push PCM frame into Opus encoder");
            }
        }

        /*
         * 停止时让Opus编码器处理内部尚未组成完整包的数据。
         */
        if (should_stop) {
            std::lock_guard<std::mutex> lock(m_audio_encoder_mutex);

            if (m_audio_encoder) {
                m_audio_encoder->Flush();
            }
        }

        while (true) {
            CODEC::EncodedAudioFrame encoded_frame;

            {
                std::lock_guard<std::mutex> lock(m_audio_encoder_mutex);

                if (!m_audio_encoder ||
                    !m_audio_encoder->PullEncoded(encoded_frame)) {
                    break;
                }
            }

            if (encoded_frame.data.empty()) {
                continue;
            }

            std::shared_ptr<rtc::Track> track;

            {
                std::lock_guard<std::mutex> lock(m_tracks_mutex);
                track = m_audio_track;
            }

            if (!track || !track->isOpen()) {
                continue;
            }

            const double relative_seconds =
                GetRelativeTimestampSeconds(
                    m_session_start_time_us,
                    encoded_frame.timestamp_us);

            rtc::FrameInfo frame_info(0u);
            frame_info.payloadType = kOpusPayloadType;
            frame_info.timestampSeconds =
                std::chrono::duration<double>(relative_seconds);

            rtc::binary sample(
                reinterpret_cast<const std::byte*>(
                    encoded_frame.data.data()),
                reinterpret_cast<const std::byte*>(
                    encoded_frame.data.data() +
                    encoded_frame.data.size()));

            try {
                track->sendFrame(std::move(sample), frame_info);
            } catch (const std::exception& exception) {
                LOG_ERROR("Failed to send Opus frame: {}",
                          exception.what());
            }
        }

        if (should_stop) {
            return;
        }
    }
}

void RtcPushTransport::Close() {
    StopPublishVideo();
    StopPublishAudio();

    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;

    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);

        video_track = std::move(m_video_track);
        audio_track = std::move(m_audio_track);
        m_tracks_initialized = false;
    }

    try {
        if (video_track) {
            video_track->close();
        }

        if (audio_track) {
            audio_track->close();
        }
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to close publish tracks: {}", exception.what());
    }

    m_session_start_time_us.store(0, std::memory_order_release);
    ClosePeerConnection();
}

} // namespace TRANSPORT