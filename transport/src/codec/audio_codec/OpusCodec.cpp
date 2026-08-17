#include "OpusCodec.h"

#include "utils/logManager.h"

#include <opus/opus.h>
#include <samplerate.h>

#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace TRANSPORT {
namespace CODEC {

namespace {

// Opus 的 RTP 时间戳固定，对非48000HZ采集音频进行重采样
constexpr std::uint32_t kOpusSampleRate = 48000;

// Opus在48kHz下单包最多支持120ms音频(48000 × 120 / 1000 = 5760)
constexpr std::uint32_t kMaxOpusFrameSamples = 5760;

// Opus编码输出缓冲区
constexpr std::size_t kMaxEncodedPacketBytes = 4000;

// 最多缓存约2秒的20ms音频包
constexpr std::size_t kMaxQueuedFrames = 100;

// 输入时间戳偏差超过100ms时认为音频发生跳变
constexpr std::int64_t kTimestampResetThresholdUs = 100000;

// Opus 只接受规定的帧时长
bool IsSupportedFrameSize(std::uint32_t frame_size_ms) noexcept
{
    return frame_size_ms == 5 || frame_size_ms == 10 ||
           frame_size_ms == 20 || frame_size_ms == 40 || frame_size_ms == 60;
}

// 创建公共Float32 PCM AudioFrame
/*
    Opus 解码器内部的 decode_buffer 会被下一次解码覆盖，因此不能直接让渲染模块长期引用它。
    这里必须为输出 AudioFrame 创建独立内存并复制 PCM
    它同时被正常解码和 PLC 补偿使用，集中实现可以避免重复两段相同的内存分配代码
*/
std::shared_ptr<RawAudioFrame> CreateRawAudioFrame(const float* samples, std::uint32_t samples_per_channel,
    std::uint32_t sample_rate, std::uint32_t channels, std::int64_t timestamp_us) {
    
    if (!samples || samples_per_channel == 0 ||
        sample_rate == 0 || channels == 0) {
        return nullptr;
    }

    const std::size_t total_samples =
        static_cast<std::size_t>(samples_per_channel) * channels;

    if (total_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        return nullptr;
    }

    const std::size_t data_size = total_samples * sizeof(float);

    auto data = std::shared_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[data_size]);

    if (!data) {
        return nullptr;
    }

    std::memcpy(data.get(), samples, data_size);

    auto frame = std::make_shared<RawAudioFrame>();

    frame->data = std::move(data);
    frame->samples = static_cast<int>(samples_per_channel);
    frame->sample_rate = static_cast<int>(sample_rate);
    frame->channels = static_cast<int>(channels);
    frame->timestamp_us = timestamp_us;

    return frame;
}

} // namespace

// Opus编码器内部状态
struct OpusCodecEncoder::EncoderContext {
    ~EncoderContext() {
        if (resampler) {
            src_delete(resampler);
            resampler = nullptr;
        }

        if (encoder) {
            opus_encoder_destroy(encoder);
            encoder = nullptr;
        }
    }

    // 将输入PCM重采样到48kHz并写入编码缓存
    bool AppendInput(const float* input, std::uint32_t input_frames) {
        if (!input || input_frames == 0) {
            return false;
        }

        const float* data_to_append = input;
        std::size_t samples_to_append = static_cast<std::size_t>(input_frames) * config.channels;

        if (resampler) {
            const double ratio = static_cast<double>(kOpusSampleRate) / config.sample_rate;
                
            /*
                添加余量是因为 libsamplerate 是流式滤波器，内部有历史状态和小数采样位置，实际生成数量可能与简单计算存在轻微差异。
                多分配一点空间可以提高一次性消费完输入的概率
            */
            const std::size_t output_frames =
                static_cast<std::size_t>(std::ceil(input_frames * ratio)) + 64;

            resample_buffer.resize(output_frames * config.channels);

            SRC_DATA src_data{};
            src_data.data_in = input;
            src_data.input_frames = static_cast<long>(input_frames);
                
            src_data.data_out = resample_buffer.data();
                
            src_data.output_frames = static_cast<long>(output_frames);
                
            src_data.src_ratio = ratio;
            src_data.end_of_input = 0;

            const int result = src_process(resampler, &src_data);
            if (result != 0) {
                LOG_ERROR("Opus encoder resampling failed: {}",
                    src_strerror(result));
                    
                return false;
            }

            if (src_data.input_frames_used != static_cast<long>(input_frames)) {
                LOG_ERROR("Opus encoder did not consume all PCM input");
                return false;
            }

            data_to_append = resample_buffer.data();

            samples_to_append = static_cast<std::size_t>(src_data.output_frames_gen) * config.channels;
        }

        if (samples_to_append == 0) {
            return true;
        }

        pcm_buffer.insert(pcm_buffer.end(), data_to_append,
            data_to_append + samples_to_append);
        return true;
    }

    /**
     * 将缓存中的完整PCM块编码为Opus
     *
     * flush为true时，最后不足一帧的数据使用静音补齐。
     */
    bool EncodeBufferedFrames(bool flush) {
        const std::size_t required_samples =
            static_cast<std::size_t>(opus_frame_samples) * config.channels;

        while (true) {
            std::size_t available_samples = pcm_buffer.size() - pcm_read_offset;

            // 剩余 PCM 数据不足一个完整 Opus 帧
            if (available_samples < required_samples) {
                if (!flush || available_samples == 0) {
                    break;
                }

                // Flush 时，将最后一个不完整帧补 0 到完整 Opus 帧
                if (pcm_read_offset > 0) {
                    pcm_buffer.erase(pcm_buffer.begin(), pcm_buffer.begin() + static_cast<std::ptrdiff_t>(pcm_read_offset));
                    pcm_read_offset = 0;
                }

                pcm_buffer.resize(required_samples, 0.0F);
                available_samples = required_samples;
            }

            const float* frame_data = pcm_buffer.data() + pcm_read_offset;

            const int encoded_bytes = opus_encode_float(
                encoder,
                frame_data,
                static_cast<int>(opus_frame_samples),
                encode_buffer.data(),
                static_cast<opus_int32>(encode_buffer.size()));

            if (encoded_bytes < 0) {
                LOG_ERROR("Opus encoding failed: {}", opus_strerror(encoded_bytes));
                return false;
            }

            EncodedAudioFrame frame;
            frame.data.assign(encode_buffer.begin(), encode_buffer.begin() + encoded_bytes);
            frame.timestamp_us = next_timestamp_us;
            frame.samples_per_channel = opus_frame_samples;
            frame.codec_type = AudioCodecType::kOpus;
                
            /*
             * 防止网络发送线程异常时队列无限增长。
             * 丢弃最旧帧可以避免延迟持续累积。
             */
            if (output_queue.size() >= kMaxQueuedFrames) {
                output_queue.pop_front();

                if (!queue_overflow_reported) {
                    LOG_WARN("Opus encoded queue overflow, dropping the oldest frame");
                    queue_overflow_reported = true;
                }
            }

            output_queue.emplace_back(std::move(frame));

            pcm_read_offset += required_samples;

            next_timestamp_us += static_cast<std::int64_t>(opus_frame_samples) * 1000000LL / kOpusSampleRate;
        }

        if (pcm_read_offset == pcm_buffer.size()) {
            pcm_buffer.clear();
            pcm_read_offset = 0;
        // 4 个 Opus 帧的数据，此时再统一清理一次
        } else if (pcm_read_offset >= static_cast<std::size_t>(opus_frame_samples) * config.channels * 4) {
            /*
             * 不在每次编码后执行vector::erase。
             * 只有累计消费较多数据后才进行一次压缩，
             * 避免每个Opus帧都搬移剩余PCM。
             */
            pcm_buffer.erase(pcm_buffer.begin(),
                pcm_buffer.begin() + static_cast<std::ptrdiff_t>(pcm_read_offset));

            pcm_read_offset = 0;
        }

        return true;
    }

    /*
    Opus 编码器对象内部还保存：
        编码参数
        上一帧音频状态
        语音预测信息
        FEC、VBR、DTX设置
    */
    OpusEncoder* encoder{nullptr}; 

    // 重采样器对象
    SRC_STATE* resampler{nullptr};

    AudioCodecConfig config{};

    // 一个 Opus 编码帧包含的“每声道采样点数”
    std::uint32_t opus_frame_samples{960};
    // 等待编码的PCM数据
    std::vector<float> pcm_buffer;
    std::size_t pcm_read_offset{0};

    //重采样的临时输出空间
    std::vector<float> resample_buffer;
    // opus_encode_float() 写入编码结果的临时字节缓冲区
    std::vector<std::uint8_t> encode_buffer;

    std::deque<EncodedAudioFrame> output_queue;

    std::int64_t next_timestamp_us{0}; // 下一帧即将编码的 Opus 音频对应的起始时间
    bool has_timestamp{false};
    bool queue_overflow_reported{false};
};

// Opus解码器内部状态

struct OpusCodecDecoder::DecoderContext
{
    ~DecoderContext() {
        if (resampler) {
            src_delete(resampler);
            resampler = nullptr;
        }

        if (decoder) {
            opus_decoder_destroy(decoder);
            decoder = nullptr;
        }
    }

    /**
     * 将 Opus 的 48kHz 解码结果转换到目标采样率
     */
    bool ConvertOutput(const float* input, std::uint32_t input_frames, const float*& output, std::uint32_t& output_frames) {
        if (!input || input_frames == 0) {
            return false;
        }

        if (!resampler) {
            output = input;
            output_frames = input_frames;
            return true;
        }

        const double ratio = static_cast<double>(config.sample_rate) / kOpusSampleRate;
        const std::size_t output_capacity = static_cast<std::size_t>(std::ceil(input_frames * ratio)) + 64;

        resample_buffer.resize(output_capacity * config.channels);

        SRC_DATA src_data{};
        src_data.data_in = input;
        src_data.input_frames = static_cast<long>(input_frames);
        src_data.data_out = resample_buffer.data();
        src_data.output_frames = static_cast<long>(output_capacity);
        src_data.src_ratio = ratio;
        src_data.end_of_input = 0;

        const int result = src_process(resampler, &src_data);

        if (result != 0) {
            LOG_ERROR("Opus decoder resampling failed: {}", src_strerror(result));
            return false;
        }

        if (src_data.input_frames_used != static_cast<long>(input_frames)) {
            LOG_ERROR("Opus decoder did not consume all decoded PCM");
            return false;
        }

        output = resample_buffer.data();
        output_frames = static_cast<std::uint32_t>(src_data.output_frames_gen);

        return output_frames > 0;
    }

    OpusDecoder* decoder{nullptr};
    SRC_STATE* resampler{nullptr};

    AudioCodecConfig config{};

    // Opus 内部 48kHz 帧长
    std::uint32_t opus_frame_samples{960};

    // 输出采样率对应的帧长
    std::uint32_t output_frame_samples{960};

    std::vector<float> decode_buffer;
    std::vector<float> resample_buffer;

    std::deque<std::shared_ptr<RawAudioFrame>> output_queue;

    std::int64_t next_timestamp_us{0};
    bool has_timestamp{false};
    bool queue_overflow_reported{false};
};

// OpusCodecEncoder

OpusCodecEncoder::OpusCodecEncoder() = default;

OpusCodecEncoder::~OpusCodecEncoder() {
    Release();
}

bool OpusCodecEncoder::Initialize(const AudioCodecConfig& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (config.sample_rate == 0 || config.channels == 0 || config.channels > 2 ||
        config.bitrate_kbps == 0 || config.bitrate_kbps > 512 ||
        config.complexity > 10 || !IsSupportedFrameSize(config.frame_size_ms) ||
        config.sample_format != AudioSampleFormat::kFloat32) {
        LOG_ERROR("Invalid Opus encoder configuration");
        return false;
    }

    auto context = std::make_unique<EncoderContext>();
    context->config = config;

    context->opus_frame_samples = kOpusSampleRate * config.frame_size_ms / 1000;

    int opus_error = OPUS_OK;

    context->encoder = opus_encoder_create(kOpusSampleRate, static_cast<int>(config.channels), OPUS_APPLICATION_VOIP, &opus_error);
    if (!context->encoder || opus_error != OPUS_OK) {
        LOG_ERROR("Failed to create Opus encoder: {}", opus_strerror(opus_error));
        return false;
    }

    int result = OPUS_OK;

    result = opus_encoder_ctl(context->encoder, OPUS_SET_BITRATE(static_cast<int>(config.bitrate_kbps * 1000)));
    if (result == OPUS_OK) {
        result = opus_encoder_ctl(context->encoder, OPUS_SET_COMPLEXITY(static_cast<int>(config.complexity)));
    }

    if (result == OPUS_OK) {
        result = opus_encoder_ctl(context->encoder, OPUS_SET_VBR(config.enable_vbr ? 1 : 0));
    }

    if (result == OPUS_OK) {
        result = opus_encoder_ctl(context->encoder, OPUS_SET_DTX(config.enable_dtx ? 1 : 0));
    }

    if (result == OPUS_OK) {
        result = opus_encoder_ctl(context->encoder, OPUS_SET_INBAND_FEC(config.enable_fec ? 1 : 0));
    }

    if (result == OPUS_OK) {
        result = opus_encoder_ctl(context->encoder, OPUS_SET_PACKET_LOSS_PERC(config.enable_fec ? 10 : 0));
    }

    if (result == OPUS_OK) {
        result = opus_encoder_ctl(context->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }

    if (result != OPUS_OK) {
        LOG_ERROR("Failed to configure Opus encoder: {}", opus_strerror(result));
        return false;
    }

    // 采样率不同需要重采样
    if (config.sample_rate != kOpusSampleRate) {
        int src_error = 0;

        context->resampler = src_new(SRC_SINC_FASTEST, static_cast<int>(config.channels), &src_error);
        if (!context->resampler || src_error != 0) {
            LOG_ERROR("Failed to create encoder resampler: {}", src_strerror(src_error));
            return false;
        }
    }

    const std::size_t frame_samples = static_cast<std::size_t>(context->opus_frame_samples) * config.channels;
    context->pcm_buffer.reserve(frame_samples * 4);
    context->encode_buffer.resize(kMaxEncodedPacketBytes);

    m_context = std::move(context);

    LOG_INFO(
        "Opus encoder initialized: input={}Hz, channels={}, frame={}ms, bitrate={}kbps",
        config.sample_rate,
        config.channels,
        config.frame_size_ms,
        config.bitrate_kbps);

    return true;
}

void OpusCodecEncoder::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_context.reset();
}

bool OpusCodecEncoder::PushInput(const std::shared_ptr<RawAudioFrame>& input)
{
    if (!input || !input->data || input->samples <= 0 ||
        input->sample_rate <= 0 || input->channels <= 0) {
        LOG_ERROR("Invalid PCM frame passed to Opus encoder");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder) {
        return false;
    }

    if (input->sample_rate != static_cast<int>(m_context->config.sample_rate) ||
        input->channels != static_cast<int>(m_context->config.channels)) {
        LOG_ERROR(
            "PCM format mismatch: input={}Hz/{}ch, expected={}Hz/{}ch",
            input->sample_rate,
            input->channels,
            m_context->config.sample_rate,
            m_context->config.channels);
        return false;
    }

    const std::size_t buffered_samples = m_context->pcm_buffer.size() - m_context->pcm_read_offset;

    const std::size_t buffered_frames = buffered_samples / m_context->config.channels;

    // 检查输入 PCM 音频帧的时间戳是否连续，如果发现时间跳变（例如麦克风切换、采集暂停恢复），
    // 则清空缓存并重置 Opus 编码器状态
    if (!m_context->has_timestamp) {
        m_context->next_timestamp_us = input->timestamp_us;
        m_context->has_timestamp = true;
    } else {
        const std::int64_t expected_timestamp =
            m_context->next_timestamp_us + static_cast<std::int64_t>(buffered_frames) * 1000000LL / kOpusSampleRate;

        const std::int64_t difference = input->timestamp_us - expected_timestamp;

        if (difference > kTimestampResetThresholdUs || difference < -kTimestampResetThresholdUs) {
            /*
             * 设备切换或采集暂停可能导致时间戳跳变。
             * 丢弃旧的不完整帧并重置编码器状态。
             */
            m_context->pcm_buffer.clear();
            m_context->pcm_read_offset = 0;

            if (m_context->resampler) {
                src_reset(m_context->resampler);
            }

            opus_encoder_ctl(m_context->encoder, OPUS_RESET_STATE);

            m_context->next_timestamp_us = input->timestamp_us;
        }
    }

    const auto* pcm = reinterpret_cast<const float*>(input->data.get());

    if (!m_context->AppendInput(pcm, static_cast<std::uint32_t>(input->samples))) {
        return false;
    }

    return m_context->EncodeBufferedFrames(false);
}

bool OpusCodecEncoder::PullEncoded(EncodedAudioFrame& output) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || m_context->output_queue.empty()) {
        return false;
    }

    output = std::move(m_context->output_queue.front());
    m_context->output_queue.pop_front();

    if (m_context->output_queue.size() < kMaxQueuedFrames / 2) {
        m_context->queue_overflow_reported = false;
    }

    return true;
}

std::size_t OpusCodecEncoder::GetPendingFrameCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_context ? m_context->output_queue.size() : 0;
}

bool OpusCodecEncoder::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder) {
        return false;
    }

    // flush 时补齐最后不足一个 Opus 帧的 PCM 数据
    const bool result = m_context->EncodeBufferedFrames(true);

    if (m_context->resampler) {
        src_reset(m_context->resampler);
    }

    return result;
}

bool OpusCodecEncoder::SetBitrate(std::uint32_t bitrate_kbps) {
    if (bitrate_kbps == 0 || bitrate_kbps > 512) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder) {
        return false;
    }

    const int result = opus_encoder_ctl(m_context->encoder, OPUS_SET_BITRATE(static_cast<int>(bitrate_kbps * 1000)));

    if (result != OPUS_OK) {
        return false;
    }

    m_context->config.bitrate_kbps = bitrate_kbps;
    return true;
}

bool OpusCodecEncoder::SetComplexity(std::uint32_t complexity) {
    if (complexity > 10) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder) {
        return false;
    }

    const int result = opus_encoder_ctl(m_context->encoder, OPUS_SET_COMPLEXITY(static_cast<int>(complexity)));
    if (result != OPUS_OK) {
        return false;
    }

    m_context->config.complexity = complexity;
    return true;
}

bool OpusCodecEncoder::SetVBR(bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder) {
        return false;
    }

    const int result = opus_encoder_ctl(m_context->encoder, OPUS_SET_VBR(enable ? 1 : 0));
    if (result != OPUS_OK) {
        return false;
    }

    m_context->config.enable_vbr = enable;
    return true;
}

bool OpusCodecEncoder::SetDTX(bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder) {
        return false;
    }

    const int result = opus_encoder_ctl(m_context->encoder, OPUS_SET_DTX(enable ? 1 : 0));
    if (result != OPUS_OK) {
        return false;
    }

    m_context->config.enable_dtx = enable;
    return true;
}

bool OpusCodecEncoder::SetFEC(bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder) {
        return false;
    }

    int result = opus_encoder_ctl(m_context->encoder, OPUS_SET_INBAND_FEC(enable ? 1 : 0));
    if (result == OPUS_OK) {
        // FEC 开启时设置预期丢包率，帮助 Opus生成冗余信息
        result = opus_encoder_ctl(m_context->encoder, OPUS_SET_PACKET_LOSS_PERC(enable ? 10 : 0));
    }

    if (result != OPUS_OK) {
        return false;
    }

    m_context->config.enable_fec = enable;
    return true;
}

std::uint32_t OpusCodecEncoder::GetFrameSizeSamples() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_context ? m_context->opus_frame_samples : 960;
}


// OpusCodecDecoder

OpusCodecDecoder::OpusCodecDecoder() = default;

OpusCodecDecoder::~OpusCodecDecoder() {
    Release();
}

bool OpusCodecDecoder::Initialize(const AudioCodecConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (config.sample_rate == 0 || config.channels == 0 ||
        config.channels > 2 || !IsSupportedFrameSize(config.frame_size_ms) ||
        config.sample_format != AudioSampleFormat::kFloat32) {
        LOG_ERROR("Invalid Opus decoder configuration");
        return false;
    }

    auto context = std::make_unique<DecoderContext>();

    context->config = config;
    context->opus_frame_samples = kOpusSampleRate * config.frame_size_ms / 1000;
    context->output_frame_samples = config.sample_rate * config.frame_size_ms / 1000;

    int opus_error = OPUS_OK;

    context->decoder = opus_decoder_create(kOpusSampleRate, static_cast<int>(config.channels), &opus_error);

    if (!context->decoder || opus_error != OPUS_OK) {
        LOG_ERROR("Failed to create Opus decoder: {}", opus_strerror(opus_error));
        return false;
    }

    // 输入采样率不是48kHz时，需要重采样到目标输出采样率
    if (config.sample_rate != kOpusSampleRate) {
        int src_error = 0;

        context->resampler = src_new(SRC_SINC_FASTEST, static_cast<int>(config.channels), &src_error);
        if (!context->resampler || src_error != 0) {
            LOG_ERROR("Failed to create decoder resampler: {}", src_strerror(src_error));
            return false;
        }
    }

    context->decode_buffer.resize(static_cast<std::size_t>(kMaxOpusFrameSamples) * config.channels);

    m_context = std::move(context);

    LOG_INFO(
        "Opus decoder initialized: output={}Hz, channels={}, frame={}ms",
        config.sample_rate,
        config.channels,
        config.frame_size_ms);

    return true;
}

void OpusCodecDecoder::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_context.reset();
}

bool OpusCodecDecoder::PushInput(const EncodedAudioFrame& input) {
    if (!input.IsValid() || input.codec_type != AudioCodecType::kOpus) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->decoder) {
        return false;
    }

    const int decoded_samples = opus_decode_float(
        m_context->decoder,
        input.data.data(),
        static_cast<opus_int32>(input.data.size()),
        m_context->decode_buffer.data(),
        static_cast<int>(kMaxOpusFrameSamples),
        0);

    if (decoded_samples < 0) {
        LOG_ERROR("Opus decoding failed: {}", opus_strerror(decoded_samples));
        return false;
    }

    const float* output_data = nullptr;
    std::uint32_t output_frames = 0;

    if (!m_context->ConvertOutput(
            m_context->decode_buffer.data(),
            static_cast<std::uint32_t>(decoded_samples),
            output_data,
            output_frames)) {
        return false;
    }

    auto frame = CreateRawAudioFrame(
        output_data,
        output_frames,
        m_context->config.sample_rate,
        m_context->config.channels,
        input.timestamp_us);

    if (!frame) {
        return false;
    }

    /*
     * 解码队列超过限制时丢弃最旧数据。
     * 实时音频优先保证低延迟，而不是无限堆积。
     */
    if (m_context->output_queue.size() >= kMaxQueuedFrames) {
        m_context->output_queue.pop_front();

        if (!m_context->queue_overflow_reported) {
            LOG_WARN("Opus decoded queue overflow, dropping the oldest frame");
            m_context->queue_overflow_reported = true;
        }
    }

    m_context->output_queue.emplace_back(std::move(frame));

    // 根据实际解码采样数推进下一帧时间戳
    m_context->next_timestamp_us = input.timestamp_us + static_cast<std::int64_t>(decoded_samples) * 1000000LL / kOpusSampleRate;

    m_context->has_timestamp = true;

    return true;
}

bool OpusCodecDecoder::PullDecoded(std::shared_ptr<RawAudioFrame>& output) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || m_context->output_queue.empty()) {
        return false;
    }

    output = std::move(m_context->output_queue.front());
    m_context->output_queue.pop_front();

    // 队列恢复正常后允许再次报告溢出
    if (m_context->output_queue.size() < kMaxQueuedFrames / 2) {
        m_context->queue_overflow_reported = false;
    }

    return true;
}

std::size_t OpusCodecDecoder::GetPendingFrameCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_context ? m_context->output_queue.size() : 0;
}

bool OpusCodecDecoder::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);

    /*
     * Opus packet是独立解码单元，
     * 不存在编码端那种不足一帧需要补零的情况。
     */
    return m_context && m_context->decoder;
}

bool OpusCodecDecoder::ConcealLostPacket(std::shared_ptr<RawAudioFrame>& output) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->decoder) {
        return false;
    }

    int decoded_samples = 0;

    // 尝试 Opus 自带 PLC
    if (m_context->config.enable_plc) {
        /*
         * Opus API规定：输入nullptr和长度0表示丢包。
         * Opus根据历史状态生成预测音频(PLC)。
         */
        decoded_samples = opus_decode_float(m_context->decoder, nullptr, 0, m_context->decode_buffer.data(), static_cast<int>(m_context->opus_frame_samples), 0);
    }

    if (!m_context->config.enable_plc || decoded_samples < 0) {
        /*
         * PLC失败时使用静音填充。
         * 防止播放线程因为缺少音频帧产生断流。
         */
        decoded_samples = static_cast<int>(m_context->opus_frame_samples);

        std::fill_n(m_context->decode_buffer.data(),
            static_cast<std::size_t>(decoded_samples) * m_context->config.channels, 0.0F);
    }

    const float* output_data = nullptr;
    std::uint32_t output_frames = 0;

    // 重采样
    if (!m_context->ConvertOutput(
            m_context->decode_buffer.data(),
            static_cast<std::uint32_t>(decoded_samples),
            output_data,
            output_frames)) {
        return false;
    }

    const std::int64_t timestamp_us = m_context->has_timestamp ? m_context->next_timestamp_us : 0;

    // output是预测或静音的PCM经过重采样和类型转换后的原始帧输出
    output = CreateRawAudioFrame(
        output_data,
        output_frames,
        m_context->config.sample_rate,
        m_context->config.channels,
        timestamp_us);

    if (!output) {
        return false;
    }

    m_context->next_timestamp_us =
        timestamp_us + static_cast<std::int64_t>(decoded_samples) * 1000000LL / kOpusSampleRate;

    m_context->has_timestamp = true;

    return true;
}

std::uint32_t OpusCodecDecoder::GetFrameSizeSamples() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_context ? m_context->output_frame_samples : 960;
}

} // namespace CODEC
} // namespace TRANSPORT