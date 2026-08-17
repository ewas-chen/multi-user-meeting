#include "AudioMixer.h"

#include "utils/logManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace RENDER {

AudioMixer::AudioMixer() = default;

AudioMixer::~AudioMixer()
{
    Uninitialize();
}

bool AudioMixer::Initialize(int sample_rate, int channels)
{
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (m_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    if (sample_rate <= 0 || channels <= 0 || channels > 8) {
        LOG_ERROR("Invalid AudioMixer parameters: sample_rate={}, channels={}", sample_rate, channels);
        return false;
    }

    if (m_mixing_thread.joinable()) {
        LOG_ERROR("AudioMixer worker thread is still joinable");
        return false;
    }

    const std::uint64_t mix_frame_count =
        static_cast<std::uint64_t>(sample_rate) * kMixChunkDurationMs / 1000U;

    const std::uint64_t rebuffer_threshold_frames =
        static_cast<std::uint64_t>(sample_rate) * kContinuousUnderrunRecoveryMs / 1000U;

    if (mix_frame_count == 0 ||
        rebuffer_threshold_frames == 0 ||
        mix_frame_count > std::numeric_limits<std::uint32_t>::max() ||
        rebuffer_threshold_frames > std::numeric_limits<std::uint32_t>::max()) {
        LOG_ERROR("AudioMixer frame count calculation failed");
        return false;
    }

    m_sample_rate = sample_rate;
    m_channels = channels;
    m_mix_frame_count = static_cast<std::uint32_t>(mix_frame_count);
    m_rebuffer_threshold_frames = static_cast<std::uint32_t>(rebuffer_threshold_frames);

    {
        std::lock_guard<std::mutex> lock(m_user_states_mutex);
        m_user_states.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);

        m_mixed_queue.clear();
        m_front_chunk_frame_offset = 0;
        m_consecutive_underrun_frames = 0;
        m_mix_cursor_us = 0;
        m_mix_cursor_initialized = false;
        m_playback_clock.Reset();
    }

    m_mixing_thread_running.store(true, std::memory_order_release);

    try {
        m_mixing_thread = std::thread(&AudioMixer::MixingThreadLoop, this);
    } catch (const std::exception& exception) {
        m_mixing_thread_running.store(false, std::memory_order_release);
        LOG_ERROR("Failed to create AudioMixer thread: {}", exception.what());
        return false;
    }

    m_initialized.store(true, std::memory_order_release);

    LOG_INFO(
        "AudioMixer initialized: sample_rate={}, channels={}, chunk_frames={}, rebuffer_frames={}",
        m_sample_rate, m_channels, m_mix_frame_count, m_rebuffer_threshold_frames);

    return true;
}

void AudioMixer::Uninitialize()
{
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) && !m_mixing_thread.joinable()) {
        return;
    }

    /*
     * 先阻止新数据进入，再停止混音线程。
     * RenderEngine应当先停止AudioRender播放回调。
     */
    m_initialized.store(false, std::memory_order_release);
    m_mixing_thread_running.store(false, std::memory_order_release);
    m_queue_cv.notify_all();

    if (m_mixing_thread.joinable()) {
        m_mixing_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_user_states_mutex);
        m_user_states.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);

        m_mixed_queue.clear();
        m_front_chunk_frame_offset = 0;
        m_consecutive_underrun_frames = 0;
        m_mix_cursor_us = 0;
        m_mix_cursor_initialized = false;
        m_playback_clock.Reset();
    }

    LOG_INFO("AudioMixer uninitialized");
}

bool AudioMixer::PushAudioData(
    const std::string& user_name,
    const std::shared_ptr<AudioFrame>& frame)
{
    if (!m_initialized.load(std::memory_order_acquire) ||
        user_name.empty() ||
        !frame ||
        !frame->IsValid() ||
        frame->timestamp_us <= 0) {
        return false;
    }

    /*
     * AudioMixer只负责时间戳对齐和混音，
     * 不在实时接收线程中执行重采样或声道转换。
     */
    if (frame->sample_rate != m_sample_rate || frame->channels != m_channels) {
        return false;
    }

    const std::int64_t arrival_timestamp_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    try {
        const auto user_state = GetOrCreateUserState(user_name);

        if (!user_state) {
            return false;
        }

        const TimestampedAudioPushResult result = user_state->buffer.Push(*frame);

        if (result == TimestampedAudioPushResult::kInvalid) {
            return false;
        }

        if (result == TimestampedAudioPushResult::kAcceptedAfterReset) {
            /*
             * PCM缓冲已经因时间戳跳变建立了新时间线。
             * 同步重置抖动估算，并用当前帧建立新的到达基准。
             */
            user_state->jitter_controller.Reset();
            user_state->jitter_controller.ObserveArrival(
                frame->timestamp_us, arrival_timestamp_us);

            /*
             * 尚未播放的混音块仍属于旧时间线，必须清除。
             * 保留现有ClearMixedQueue行为，不增加新的重缓冲逻辑。
             */
            ClearMixedQueue();
        } else {
            /*
             * 控制器只记录媒体间隔和本地到达间隔。
             * 目标缓存的变化不会在这里清队列或重置播放时钟。
             */
            user_state->jitter_controller.ObserveArrival(
                frame->timestamp_us, arrival_timestamp_us);
        }

        /*
         * kDroppedLate表示有效但已经过播放位置的音频帧。
         * 缓冲区已正确处理，因此输入仍按成功返回。
         */
        m_queue_cv.notify_one();
        return true;
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to push audio for user {}: {}", user_name, exception.what());
        return false;
    }
}

std::uint32_t AudioMixer::PopAudio(float* output, std::uint32_t frame_count) noexcept
{
    if (!output || frame_count == 0) {
        return 0;
    }

    const int channels = m_channels;

    if (channels <= 0) {
        return 0;
    }

    const std::size_t channel_count = static_cast<std::size_t>(channels);

    if (static_cast<std::size_t>(frame_count) >
        std::numeric_limits<std::size_t>::max() / channel_count) {
        return 0;
    }

    const std::size_t required_samples =
        static_cast<std::size_t>(frame_count) * channel_count;

    /*
     * 播放回调要求输出缓冲区始终被完整初始化。
     * 未被有效混音数据覆盖的部分保持为静音。
     */
    std::memset(output, 0, required_samples * sizeof(float));

    if (!m_initialized.load(std::memory_order_acquire)) {
        return frame_count;
    }

    bool notify_mixing_thread = false;

    try {
        std::lock_guard<std::mutex> lock(m_queue_mutex);

        std::uint32_t written_frames = 0;
        std::optional<std::int64_t> playback_end_timestamp_us;

        while (written_frames < frame_count && !m_mixed_queue.empty()) {
            MixedAudioChunk& chunk = m_mixed_queue.front();

            const std::size_t data_frames = chunk.data.size() / channel_count;
            const std::size_t valid_frames =
                std::min(data_frames, static_cast<std::size_t>(chunk.frame_count));

            if (m_front_chunk_frame_offset >= valid_frames) {
                m_mixed_queue.pop_front();
                m_front_chunk_frame_offset = 0;
                notify_mixing_thread = true;
                continue;
            }

            const std::size_t available_frames =
                valid_frames - m_front_chunk_frame_offset;

            const std::size_t output_frames_left =
                static_cast<std::size_t>(frame_count - written_frames);

            const std::size_t copy_frames =
                std::min(available_frames, output_frames_left);

            const std::size_t source_sample_offset =
                m_front_chunk_frame_offset * channel_count;

            const std::size_t output_sample_offset =
                static_cast<std::size_t>(written_frames) * channel_count;

            const std::size_t copy_samples = copy_frames * channel_count;

            std::memcpy(
                output + output_sample_offset,
                chunk.data.data() + source_sample_offset,
                copy_samples * sizeof(float));

            m_front_chunk_frame_offset += copy_frames;
            written_frames += static_cast<std::uint32_t>(copy_frames);

            /*
             * 播放时钟表示已经提交给声卡的最后一个
             * 有效媒体采样帧之后的位置。
             */
            playback_end_timestamp_us =
                chunk.start_timestamp_us +
                FramesToMicroseconds(m_front_chunk_frame_offset, m_sample_rate);

            if (m_front_chunk_frame_offset >= valid_frames) {
                m_mixed_queue.pop_front();
                m_front_chunk_frame_offset = 0;
                notify_mixing_thread = true;
            }
        }

        const std::uint32_t silent_frames = frame_count - written_frames;

        if (silent_frames == 0) {
            /*
             * 本次播放回调全部由有效混音数据覆盖，
             * 结束连续欠载状态。
             */
            m_consecutive_underrun_frames = 0;

            if (playback_end_timestamp_us) {
                m_playback_clock.Update(*playback_end_timestamp_us);
            }
        } else {
            const auto current_playback_timestamp_us =
                m_playback_clock.GetTimestampUs();

            /*
             * 音频从未开始播放时，不把声卡启动阶段输出的
             * 静音统计为网络欠载，也不能凭空建立媒体时钟。
             */
            if (playback_end_timestamp_us || current_playback_timestamp_us) {
                if (written_frames > 0) {
                    /*
                     * 本回调先输出了有效音频，连续欠载从
                     * 有效音频结束的位置重新开始计算。
                     */
                    m_consecutive_underrun_frames = silent_frames;
                } else {
                    const std::uint64_t maximum_value =
                        std::numeric_limits<std::uint64_t>::max();

                    if (m_consecutive_underrun_frames >
                        maximum_value - silent_frames) {
                        m_consecutive_underrun_frames = maximum_value;
                    } else {
                        m_consecutive_underrun_frames += silent_frames;
                    }
                }

                if (m_consecutive_underrun_frames >=
                    m_rebuffer_threshold_frames) {
                    /*
                     * 持续欠载说明当前播放时间线已经追到
                     * 网络输入前方，继续推进会使后续音频
                     * 和视频被持续判定为过期。
                     */
                    EnterRebufferingLocked();
                    notify_mixing_thread = true;
                } else {
                    std::int64_t next_playback_timestamp_us = 0;

                    if (playback_end_timestamp_us) {
                        next_playback_timestamp_us =
                            *playback_end_timestamp_us +
                            FramesToMicroseconds(silent_frames, m_sample_rate);
                    } else {
                        next_playback_timestamp_us =
                            *current_playback_timestamp_us +
                            FramesToMicroseconds(frame_count, m_sample_rate);
                    }

                    /*
                     * 短暂欠载期间声卡仍然消费静音，因此时钟
                     * 暂时继续推进。同时跳过该静音对应的混音
                     * 区间，防止迟到音频随后被重复播放。
                     */
                    m_playback_clock.Update(next_playback_timestamp_us);

                    if (m_mix_cursor_initialized) {
                        /*
                         * 额外推进1微秒可使正在锁外生成的旧块
                         * 在重新入队时因游标不一致而被丢弃。
                         */
                        if (next_playback_timestamp_us <
                            std::numeric_limits<std::int64_t>::max()) {
                            m_mix_cursor_us = next_playback_timestamp_us + 1;
                        } else {
                            m_mix_cursor_us = next_playback_timestamp_us;
                        }

                        notify_mixing_thread = true;
                    }
                }
            } else {
                m_consecutive_underrun_frames = 0;
            }
        }
    } catch (...) {
        /*
         * 播放回调不能向外抛出异常。
         * output已经清零，可以安全输出静音。
         */
        return frame_count;
    }

    if (notify_mixing_thread) {
        m_queue_cv.notify_all();
    }

    return frame_count;
}

void AudioMixer::RemoveUserBuffer(const std::string& user_name)
{
    if (user_name.empty()) {
        return;
    }

    std::shared_ptr<UserAudioState> removed_state;

    {
        std::lock_guard<std::mutex> lock(m_user_states_mutex);

        const auto iterator = m_user_states.find(user_name);

        if (iterator == m_user_states.end()) {
            return;
        }

        removed_state = iterator->second;
        m_user_states.erase(iterator);
    }

    if (removed_state) {
        removed_state->buffer.Reset();
        removed_state->jitter_controller.Reset();
    }

    /*
     * 尚未播放的混音块可能包含被删除用户的声音。
     */
    ClearMixedQueue();
}

std::optional<std::int64_t>
AudioMixer::GetPlaybackTimestampUs() const noexcept
{
    return m_playback_clock.GetTimestampUs();
}

void AudioMixer::ResetUserTimeline(const std::string& user_name)
{
    if (user_name.empty()) {
        return;
    }

    std::shared_ptr<UserAudioState> user_state;

    {
        std::lock_guard<std::mutex> lock(m_user_states_mutex);

        const auto iterator = m_user_states.find(user_name);

        if (iterator == m_user_states.end()) {
            return;
        }

        user_state = iterator->second;
    }

    if (user_state) {
        user_state->buffer.Reset();
        user_state->jitter_controller.Reset();
    }

    /*
     * 单个用户重置时保留整体播放时钟，
     * 其他用户仍可以沿公共时间线继续播放。
     */
    ClearMixedQueue();
}

void AudioMixer::ResetPlaybackTimeline()
{
    const auto user_states = GetUserStatesSnapshot();

    for (const auto& state : user_states) {
        if (!state) {
            continue;
        }

        state->buffer.Reset();
        state->jitter_controller.Reset();
    }

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);

        m_mixed_queue.clear();
        m_front_chunk_frame_offset = 0;
        m_consecutive_underrun_frames = 0;
        m_mix_cursor_us = 0;
        m_mix_cursor_initialized = false;
        m_playback_clock.Reset();
    }

    /*
     * 新音频达到各自的启动缓存目标后，
     * 混音线程会自动重新建立公共时间线。
     */
    m_queue_cv.notify_all();
}

std::shared_ptr<AudioMixer::UserAudioState>
AudioMixer::GetOrCreateUserState(const std::string& user_name)
{
    std::lock_guard<std::mutex> lock(m_user_states_mutex);

    if (!m_initialized.load(std::memory_order_acquire)) {
        return nullptr;
    }

    const auto iterator = m_user_states.find(user_name);

    if (iterator != m_user_states.end()) {
        return iterator->second;
    }

    auto state = std::make_shared<UserAudioState>(
        m_sample_rate,
        m_channels,
        kMaxUserBufferDurationMs,
        kMaxGapFillDurationMs,
        kDiscontinuityThresholdMs);

    if (!state->buffer.IsConfigured()) {
        return nullptr;
    }

    m_user_states.emplace(user_name, state);
    return state;
}

std::vector<std::shared_ptr<AudioMixer::UserAudioState>>
AudioMixer::GetUserStatesSnapshot() const
{
    std::vector<std::shared_ptr<UserAudioState>> user_states;

    std::lock_guard<std::mutex> lock(m_user_states_mutex);
    user_states.reserve(m_user_states.size());

    for (const auto& [user_name, state] : m_user_states) {
        (void)user_name;

        if (state) {
            user_states.push_back(state);
        }
    }

    return user_states;
}

std::optional<std::int64_t>
AudioMixer::FindInitialMixTimestampUs() const
{
    std::optional<std::int64_t> initial_timestamp_us;
    const auto user_states = GetUserStatesSnapshot();

    for (const auto& state : user_states) {
        if (!state) {
            continue;
        }

        const std::int64_t target_buffer_us =
            state->jitter_controller.GetTargetBufferDurationUs();

        const std::size_t target_buffer_frames =
            MicrosecondsToFrames(target_buffer_us, m_sample_rate);

        if (target_buffer_frames == 0 ||
            state->buffer.GetBufferedFrameCount() < target_buffer_frames) {
            continue;
        }

        const auto first_timestamp_us = state->buffer.GetFirstTimestampUs();

        if (!first_timestamp_us) {
            continue;
        }

        if (!initial_timestamp_us ||
            *first_timestamp_us < *initial_timestamp_us) {
            initial_timestamp_us = *first_timestamp_us;
        }
    }

    return initial_timestamp_us;
}

bool AudioMixer::HasBufferedAudioForChunk(
    std::int64_t chunk_timestamp_us) const
{
    if (chunk_timestamp_us <= 0 ||
        m_mix_frame_count == 0 ||
        m_sample_rate <= 0) {
        return false;
    }

    const std::int64_t chunk_duration_us =
        FramesToMicroseconds(m_mix_frame_count, m_sample_rate);

    if (chunk_duration_us <= 0 ||
        chunk_timestamp_us >
            std::numeric_limits<std::int64_t>::max() - chunk_duration_us) {
        return false;
    }

    const std::int64_t chunk_end_timestamp_us =
        chunk_timestamp_us + chunk_duration_us;

    const auto user_states = GetUserStatesSnapshot();

    for (const auto& state : user_states) {
        if (!state) {
            continue;
        }

        const auto first_timestamp_us =
            state->buffer.GetFirstTimestampUs();

        const auto end_timestamp_us =
            state->buffer.GetEndTimestampUs();

        /*
         * 至少一个用户必须完整覆盖目标混音区间。
         * 只检查“有数据”仍可能把未来尚未到达的部分
         * 预先固化成静音。
         */
        if (first_timestamp_us &&
            end_timestamp_us &&
            *first_timestamp_us <= chunk_timestamp_us &&
            *end_timestamp_us >= chunk_end_timestamp_us) {
            return true;
        }
    }

    return false;
}

bool AudioMixer::CanProduceMixChunk() const
{
    if (!m_mix_cursor_initialized) {
        return FindInitialMixTimestampUs().has_value();
    }

    return HasBufferedAudioForChunk(m_mix_cursor_us);
}

void AudioMixer::EnterRebufferingLocked() noexcept
{
    /*
     * 调用方持有m_queue_mutex。
     * 用户PCM继续保留，重新达到各自的动态启动目标后，
     * 从最早有效时间戳建立新的播放时间线。
     */
    m_mixed_queue.clear();
    m_front_chunk_frame_offset = 0;
    m_consecutive_underrun_frames = 0;
    m_mix_cursor_us = 0;
    m_mix_cursor_initialized = false;
    m_playback_clock.Reset();
}

void AudioMixer::ClearMixedQueue()
{
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);

        const auto playback_timestamp_us =
            m_playback_clock.GetTimestampUs();

        m_mixed_queue.clear();
        m_front_chunk_frame_offset = 0;
        m_consecutive_underrun_frames = 0;

        /*
         * 单个用户变化时尽量保留公共播放时钟。
         * 增加1微秒用于使正在生成的旧时间线混音块失效。
         */
        if (playback_timestamp_us) {
            if (*playback_timestamp_us <
                std::numeric_limits<std::int64_t>::max()) {
                m_mix_cursor_us = *playback_timestamp_us + 1;
            } else {
                m_mix_cursor_us = *playback_timestamp_us;
            }

            m_mix_cursor_initialized = true;
        } else {
            m_mix_cursor_us = 0;
            m_mix_cursor_initialized = false;
        }
    }

    m_queue_cv.notify_all();
}

void AudioMixer::ClampAndNormalize(
    float* samples,
    std::size_t sample_count) noexcept
{
    if (!samples || sample_count == 0) {
        return;
    }

    float peak = 0.0F;

    for (std::size_t index = 0; index < sample_count; ++index) {
        /*
         * 异常输入中的NaN或Inf不能进入声卡缓冲，
         * 否则可能产生明显噪音。
         */
        if (!std::isfinite(samples[index])) {
            samples[index] = 0.0F;
            continue;
        }

        peak = std::max(peak, std::abs(samples[index]));
    }

    if (peak <= 1.0F) {
        return;
    }

    const float scale = 1.0F / peak;

    for (std::size_t index = 0; index < sample_count; ++index) {
        samples[index] *= scale;
    }
}

std::int64_t AudioMixer::FramesToMicroseconds(
    std::uint64_t frames,
    int sample_rate) noexcept
{
    if (sample_rate <= 0 || frames == 0) {
        return 0;
    }

    const std::uint64_t rate = static_cast<std::uint64_t>(sample_rate);
    const std::uint64_t whole_seconds = frames / rate;
    const std::uint64_t remaining_frames = frames % rate;

    const std::uint64_t maximum_microseconds =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    if (whole_seconds > maximum_microseconds / 1'000'000ULL) {
        return std::numeric_limits<std::int64_t>::max();
    }

    const std::uint64_t microseconds =
        whole_seconds * 1'000'000ULL +
        remaining_frames * 1'000'000ULL / rate;

    if (microseconds > maximum_microseconds) {
        return std::numeric_limits<std::int64_t>::max();
    }

    return static_cast<std::int64_t>(microseconds);
}

std::size_t AudioMixer::MicrosecondsToFrames(
    std::int64_t duration_us,
    int sample_rate) noexcept
{
    if (duration_us <= 0 || sample_rate <= 0) {
        return 0;
    }

    const std::uint64_t duration =
        static_cast<std::uint64_t>(duration_us);

    const std::uint64_t rate =
        static_cast<std::uint64_t>(sample_rate);

    const std::uint64_t whole_seconds =
        duration / 1'000'000ULL;

    const std::uint64_t remaining_us =
        duration % 1'000'000ULL;

    const std::uint64_t maximum_frames =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max());

    if (whole_seconds > maximum_frames / rate) {
        return std::numeric_limits<std::size_t>::max();
    }

    const std::uint64_t whole_frames =
        whole_seconds * rate;

    /*
     * 向上取整，保证实际累计的PCM时长不会低于目标缓存时长。
     */
    const std::uint64_t remaining_frames =
        (remaining_us * rate + 999'999ULL) /
        1'000'000ULL;

    if (remaining_frames > maximum_frames - whole_frames) {
        return std::numeric_limits<std::size_t>::max();
    }

    return static_cast<std::size_t>(
        whole_frames + remaining_frames);
}

void AudioMixer::MixingThreadLoop()
{
    const std::size_t samples_per_chunk =
        static_cast<std::size_t>(m_mix_frame_count) *
        static_cast<std::size_t>(m_channels);

    const std::int64_t chunk_duration_us =
        FramesToMicroseconds(m_mix_frame_count, m_sample_rate);

    if (samples_per_chunk == 0 || chunk_duration_us <= 0) {
        return;
    }

    std::vector<float> mixed_samples(samples_per_chunk, 0.0F);
    std::vector<float> user_samples(samples_per_chunk, 0.0F);
    std::vector<std::shared_ptr<UserAudioState>> user_states;

    while (true) {
        std::int64_t chunk_timestamp_us = 0;
        std::int64_t expected_next_cursor_us = 0;

        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);

            m_queue_cv.wait(lock, [this]() {
                if (!m_mixing_thread_running.load(
                        std::memory_order_acquire)) {
                    return true;
                }

                return m_mixed_queue.size() < kPreMixedChunkCount &&
                       CanProduceMixChunk();
            });

            if (!m_mixing_thread_running.load(
                    std::memory_order_acquire)) {
                return;
            }

            if (!m_mix_cursor_initialized) {
                const auto initial_timestamp_us =
                    FindInitialMixTimestampUs();

                if (!initial_timestamp_us) {
                    continue;
                }

                /*
                 * 只有至少一个用户达到其当前启动缓存目标后，
                 * 才建立公共混音时间线。
                 */
                m_mix_cursor_us = *initial_timestamp_us;
                m_mix_cursor_initialized = true;
            }

            /*
             * 时间线建立后，每一个目标区间仍必须由至少
             * 一个真实用户缓冲完整覆盖。
             */
            if (!HasBufferedAudioForChunk(m_mix_cursor_us)) {
                continue;
            }

            chunk_timestamp_us = m_mix_cursor_us;

            if (chunk_timestamp_us >
                std::numeric_limits<std::int64_t>::max() -
                    chunk_duration_us) {
                EnterRebufferingLocked();
                continue;
            }

            expected_next_cursor_us =
                chunk_timestamp_us + chunk_duration_us;

            /*
             * 先预留目标区间。生成期间发生欠载或时间线
             * 重置时，游标会变化，旧块将无法重新入队。
             */
            m_mix_cursor_us = expected_next_cursor_us;
        }

        user_states = GetUserStatesSnapshot();

        std::fill(
            mixed_samples.begin(),
            mixed_samples.end(),
            0.0F);

        bool has_complete_audio = false;

        /*
         * 所有用户读取同一媒体时间段。
         * 没有覆盖该区间的用户由TimestampedAudioBuffer补静音。
         */
        for (const auto& state : user_states) {
            if (!state) {
                continue;
            }

            const TimestampedAudioReadResult result =
                state->buffer.ReadAt(
                    chunk_timestamp_us,
                    user_samples.data(),
                    m_mix_frame_count);

            if (!result.HasAudio()) {
                continue;
            }

            if (result.copied_frames == m_mix_frame_count &&
                result.leading_silence_frames == 0 &&
                result.trailing_silence_frames == 0) {
                has_complete_audio = true;
            }

            for (std::size_t index = 0;
                 index < samples_per_chunk;
                 ++index) {
                mixed_samples[index] += user_samples[index];
            }
        }

        /*
         * 正常情况下前面的覆盖检查保证这里至少有一个
         * 完整用户。若并发重置使数据消失，则沿用原有
         * 重新缓冲行为，不能把该区间作为静音送入队列。
         */
        if (!has_complete_audio) {
            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);

                if (m_mix_cursor_initialized &&
                    m_mix_cursor_us == expected_next_cursor_us) {
                    EnterRebufferingLocked();
                }
            }

            m_queue_cv.notify_all();
            continue;
        }

        ClampAndNormalize(mixed_samples.data(), mixed_samples.size());

        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);

            if (!m_mixing_thread_running.load(
                    std::memory_order_acquire)) {
                return;
            }

            /*
             * 欠载恢复、用户重置或整体时间线重建可能在
             * 混音计算期间修改游标。旧块不能重新入队。
             */
            if (!m_mix_cursor_initialized ||
                m_mix_cursor_us != expected_next_cursor_us) {
                continue;
            }

            if (m_mixed_queue.size() >= kPreMixedChunkCount) {
                continue;
            }

            m_mixed_queue.emplace_back(
                chunk_timestamp_us,
                m_mix_frame_count,
                mixed_samples);
        }
    }
}

} // namespace RENDER