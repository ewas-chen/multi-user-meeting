#include "H264Codec.h"

#include "utils/logManager.h"

#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <wels/codec_def.h>

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace TRANSPORT {
namespace CODEC {

// ============================================================
// 编码器内部状态
// ============================================================

struct H264Encoder::EncoderContext {
    ~EncoderContext()
    {
        if (!encoder) {
            return;
        }

        if (initialized) {
            encoder->Uninitialize();
        }

        WelsDestroySVCEncoder(encoder);
        encoder = nullptr;
    }

    ISVCEncoder* encoder{nullptr};
    VideoCodecConfig config{};
    bool initialized{false};
};

// ============================================================
// 解码器内部状态
// ============================================================

struct H264Decoder::DecoderContext {
    ~DecoderContext()
    {
        if (!decoder) {
            return;
        }

        if (initialized) {
            decoder->Uninitialize();
        }

        WelsDestroyDecoder(decoder);
        decoder = nullptr;
    }

    ISVCDecoder* decoder{nullptr};
    VideoCodecConfig config{};
    bool initialized{false};
    bool needs_keyframe{false};
};

// ============================================================
// H264Encoder
// ============================================================

H264Encoder::H264Encoder() = default;

H264Encoder::~H264Encoder()
{
    Release();
}

bool H264Encoder::Initialize(const VideoCodecConfig& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    /*
     * I420的U、V分辨率是Y的一半，因此宽高必须是偶数。
     */
    if (config.width <= 0 || config.height <= 0 ||
        (config.width % 2) != 0 || (config.height % 2) != 0 || config.framerate <= 0) {
        LOG_ERROR("Invalid H264 encoder resolution or framerate");
        return false;
    }

    if (config.min_bitrate_kbps == 0 ||
        config.target_bitrate_kbps < config.min_bitrate_kbps ||
        config.target_bitrate_kbps > config.max_bitrate_kbps ||
        config.max_bitrate_kbps > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 1000)) {
        LOG_ERROR("Invalid H264 encoder bitrate configuration");
        return false;
    }

    auto context = std::make_unique<EncoderContext>();
    context->config = config;

    if (WelsCreateSVCEncoder(&context->encoder) != 0 || !context->encoder) {
        LOG_ERROR("WelsCreateSVCEncoder failed");
        return false;
    }

    /*
     * 使用SEncParamExt是因为它可以设置码率控制、线程数、
     * 关键帧周期和空间层参数, 是 OpenH264 定义的一个编码参数结构体
     */
    SEncParamExt parameter{};

    // 让 OpenH264 填充一套默认配置
    if (context->encoder->GetDefaultParams(&parameter) != 0) {
        LOG_ERROR("OpenH264 GetDefaultParams failed");
        return false;
    }

    parameter.iUsageType = CAMERA_VIDEO_REAL_TIME; // 编码场景:摄像头实时视频
    parameter.iPicWidth = config.width;
    parameter.iPicHeight = config.height;
    parameter.fMaxFrameRate = static_cast<float>(config.framerate);

    // 目标码率
    parameter.iTargetBitrate = static_cast<int>(config.target_bitrate_kbps * 1000);

    // 最大码率
    parameter.iMaxBitrate = static_cast<int>(config.max_bitrate_kbps * 1000);

    parameter.iRCMode = RC_BITRATE_MODE; // 码率控制模式:以目标码率控制
    parameter.iComplexityMode = LOW_COMPLEXITY; // 编码复杂度
    parameter.bEnableFrameSkip = config.enable_frame_dropping; // 是否允许跳帧

    /*
     * 当前只编码一层普通H.264，不启用空间SVC或时间SVC
        Spatial Layer（空间层）:不同分辨率
        Temporal Layer（时间层）:不同帧率
        可根据网络情况选择不同层
     */
    parameter.iSpatialLayerNum = 1; // 不启用分层
    parameter.iTemporalLayerNum = 1;
    parameter.uiIntraPeriod = config.keyframe_interval; // 关键帧间隔
    parameter.iMultipleThreadIdc = static_cast<int>(config.threads); // 编码线程数量

    // 空间层配置
    SSpatialLayerConfig& layer = parameter.sSpatialLayers[0];

    layer.iVideoWidth = config.width;
    layer.iVideoHeight = config.height;
    layer.fFrameRate = static_cast<float>(config.framerate);

    layer.iSpatialBitrate =
        static_cast<int>(config.target_bitrate_kbps * 1000);

    layer.iMaxSpatialBitrate =
        static_cast<int>(config.max_bitrate_kbps * 1000);

    /*
     * WebRTC兼容性优先，使用Baseline Profile。
     */
    /*
        H264 Profile表示编码能力集合
        Baseline：简单，解码要求低，延迟低
    */ 
    layer.uiProfileIdc = PRO_BASELINE; 
    // Level限制：最大分辨率，最大码率，最大帧率等，UNKNOWN让编码器自动判断
    layer.uiLevelIdc = LEVEL_UNKNOWN;
    layer.iDLayerQp = 0;

    if (context->encoder->InitializeExt(&parameter) != 0) {
        LOG_ERROR("OpenH264 encoder InitializeExt failed");
        return false;
    }

    context->initialized = true;

    int input_format = videoFormatI420;

    if (context->encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &input_format) != 0) {
        LOG_ERROR("Failed to set OpenH264 input format to I420");
        return false;
    }

    m_context = std::move(context);

    LOG_INFO(
        "H264 encoder initialized: {}x{}@{}fps, bitrate={}kbps",
        config.width,
        config.height,
        config.framerate,
        config.target_bitrate_kbps);

    return true;
}

void H264Encoder::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_context.reset();
}

bool H264Encoder::Encode(const std::shared_ptr<RawVideoFrame>& input, EncodedVideoFrame& output) {
    if (!input || !input->IsValid()) {
        LOG_ERROR("Invalid I420 frame passed to H264 encoder");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder || !m_context->initialized) {
        LOG_ERROR("H264 encoder is not initialized");
        return false;
    }

    if (input->width != m_context->config.width || input->height != m_context->config.height) {
        LOG_ERROR(
            "I420 frame size mismatch: input={}x{}, expected={}x{}",
            input->width,
            input->height,
            m_context->config.width,
            m_context->config.height);
        return false;
    }

    /*
     * 公共I420Frame使用紧密排列的三平面数据：
     *
     * Y stride = width
     * U stride = width / 2
     * V stride = width / 2
     */
    SSourcePicture picture{}; // SSourcePicture 是 OpenH264 编码接口中的输入视频帧结构体
    picture.iColorFormat = videoFormatI420;
    picture.iPicWidth = input->width;
    picture.iPicHeight = input->height;

    picture.iStride[0] = input->width;
    picture.iStride[1] = input->width / 2;
    picture.iStride[2] = input->width / 2;

    picture.pData[0] = input->data[0].get();
    picture.pData[1] = input->data[1].get();
    picture.pData[2] = input->data[2].get();

    /*
     * OpenH264使用毫秒时间戳，公共帧使用微秒。
     */
    picture.uiTimeStamp = static_cast<long long>(input->timestamp_us / 1000);

    SFrameBSInfo frame_info{};

    const int result =
        m_context->encoder->EncodeFrame(&picture, &frame_info);

    if (result != 0) {
        LOG_ERROR("OpenH264 EncodeFrame failed: {}", result);
        return false;
    }

    /*
     * 重用调用方output的vector容量，减少连续编码时重复分配。
     */
    output.data.clear();
    output.width = input->width;
    output.height = input->height;
    output.timestamp_us = input->timestamp_us;
    output.codec_type = VideoCodecType::kH264;
    output.is_keyframe = false;
    output.is_skipped = false;

    /*
     * 码率控制可能要求编码器主动跳帧。
     * 跳帧不是编码失败，只是当前帧没有输出数据。
     */
    if (frame_info.eFrameType == videoFrameTypeSkip) {
        output.is_skipped = true;
        return true;
    }

    if (frame_info.eFrameType == videoFrameTypeInvalid) {
        LOG_ERROR("OpenH264 returned an invalid frame");
        return false;
    }

    /*
     * 只有IDR帧能够在完全没有前面参考帧时独立恢复解码。
     */
    output.is_keyframe = frame_info.eFrameType == videoFrameTypeIDR;

    /*
     * OpenH264可能输出多个Layer，每个Layer又包含多个NAL：
     * H.264码流由多个NAL组成，一个 NAL 就是一小段 H.264 数据
     * SPS、PPS、IDR、普通Slice等都必须按原顺序保留。
     * OpenH264输出已经是Annex-B格式，不需要手动添加起始码。
     */
    std::size_t total_size = 0;

    // 遍历所有编码层（之前设置1）
    for (int layer_index = 0; layer_index < frame_info.iLayerNum; ++layer_index) {
         
        const SLayerBSInfo& layer = frame_info.sLayerInfo[layer_index];

        if (layer.iNalCount <= 0) {
            continue; // 当前层没有NAL数据
        }

        // pBsBuf：NAL数据缓冲区； pNalLengthInByte：每个NAL长度数组
        if (!layer.pBsBuf || !layer.pNalLengthInByte) {
            LOG_ERROR("OpenH264 returned invalid NAL data");
            return false;
        }

        for (int nal_index = 0; nal_index < layer.iNalCount; ++nal_index) {
            const int nal_size = layer.pNalLengthInByte[nal_index];

            // 检查NAL大小是否合法
            if (nal_size <= 0 || total_size > std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(nal_size)) {
                LOG_ERROR("OpenH264 returned invalid NAL size");
                return false;
            }

            total_size += static_cast<std::size_t>(nal_size);
        }
    }

    if (total_size == 0) {
        LOG_ERROR("OpenH264 produced an empty access unit");
        return false;
    }

    output.data.resize(total_size);

    std::uint8_t* destination = output.data.data();

    for (int layer_index = 0; layer_index < frame_info.iLayerNum; ++layer_index) {
        const SLayerBSInfo& layer = frame_info.sLayerInfo[layer_index];

        if (layer.iNalCount <= 0) {
            continue;
        }

        const std::uint8_t* source = layer.pBsBuf;

        for (int nal_index = 0; nal_index < layer.iNalCount; ++nal_index) {
             
            const std::size_t nal_size = static_cast<std::size_t>(layer.pNalLengthInByte[nal_index]);

            std::memcpy(destination, source, nal_size);

            destination += nal_size;
            source += nal_size;
        }
    }

    return true;
}

bool H264Encoder::SetBitrate(std::uint32_t bitrate_kbps) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder || !m_context->initialized) {
        return false;
    }

    if (bitrate_kbps < m_context->config.min_bitrate_kbps ||
        bitrate_kbps > m_context->config.max_bitrate_kbps) {
        LOG_ERROR(
            "H264 bitrate {}kbps is outside configured range {}-{}kbps",
            bitrate_kbps,
            m_context->config.min_bitrate_kbps,
            m_context->config.max_bitrate_kbps);
        return false;
    }

    /*
     * ENCODER_OPTION_BITRATE要求传入SBitrateInfo，
     * 不能直接传递一个int。
     */
    SBitrateInfo bitrate_info{};
    bitrate_info.iLayer = SPATIAL_LAYER_ALL;
    bitrate_info.iBitrate = static_cast<int>(bitrate_kbps * 1000);

    if (m_context->encoder->SetOption(ENCODER_OPTION_BITRATE, &bitrate_info) != 0) {
        LOG_ERROR("Failed to update H264 bitrate");
        return false;
    }

    m_context->config.target_bitrate_kbps = bitrate_kbps;
    return true;
}

bool H264Encoder::SetFramerate(std::uint32_t framerate) {
    if (framerate == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder || !m_context->initialized) {
        return false;
    }

    float value = static_cast<float>(framerate);

    if (m_context->encoder->SetOption(ENCODER_OPTION_FRAME_RATE, &value) != 0) {
        LOG_ERROR("Failed to update H264 framerate");
        return false;
    }

    m_context->config.framerate = static_cast<int>(framerate);
    return true;
}

bool H264Encoder::RequestKeyframe()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->encoder || !m_context->initialized) {
        return false;
    }

    /*
     * ForceIntraFrame(true)要求下一次编码输出IDR帧。
     */
    const int result = m_context->encoder->ForceIntraFrame(true);

    if (result != 0) {
        LOG_ERROR("Failed to request H264 IDR frame: {}", result);
        return false;
    }

    return true;
}

// ============================================================
// H264Decoder
// ============================================================

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder()
{
    Release();
}

bool H264Decoder::Initialize(const VideoCodecConfig& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto context = std::make_unique<DecoderContext>();
    context->config = config;

    if (WelsCreateDecoder(&context->decoder) != 0 || !context->decoder) {
        LOG_ERROR("WelsCreateDecoder failed");
        return false;
    }

    /*
     * 解码器从SPS中读取真实分辨率，因此不要求远端视频尺寸
     * 必须与config中的本地发布尺寸一致。
     */
    SDecodingParam parameter{};
    parameter.sVideoProperty.size = sizeof(SVideoProperty);
    parameter.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    parameter.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    parameter.bParseOnly = false;

    if (context->decoder->Initialize(&parameter) != 0) {
        LOG_ERROR("OpenH264 decoder Initialize failed");
        return false;
    }

    context->initialized = true;
    m_context = std::move(context);

    LOG_INFO("H264 decoder initialized");
    return true;
}

void H264Decoder::Release()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_context.reset();
}

bool H264Decoder::Decode(const EncodedVideoFrame& input,
                         std::shared_ptr<RawVideoFrame>& output)
{
    output.reset();

    if (input.data.empty() ||
        input.codec_type != VideoCodecType::kH264 ||
        input.data.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max())) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_context || !m_context->decoder || !m_context->initialized) {
        LOG_ERROR("H264 decoder is not initialized");
        return false;
    }

    unsigned char* planes[3] = {nullptr, nullptr, nullptr};
    SBufferInfo buffer_info{};

    const DECODING_STATE state =
        m_context->decoder->DecodeFrameNoDelay(
            input.data.data(),
            static_cast<int>(input.data.size()),
            planes,
            &buffer_info);

    const int state_bits = static_cast<int>(state);

    const int fatal_errors =
        dsInvalidArgument |
        dsInitialOptExpected |
        dsOutOfMemory |
        dsDstBufNeedExpan;

    if ((state_bits & fatal_errors) != 0) {
        m_context->needs_keyframe = true;

        LOG_ERROR(
            "OpenH264 fatal decoding error: {}",
            state_bits);
        return false;
    }

    /*
     * 参考帧、参数集或码流损坏后，当前解码状态可能无法
     * 仅依靠后续P帧恢复，需要请求新的IDR关键帧。
     */
    const int reference_errors =
        dsRefLost |
        dsBitstreamError |
        dsDepLayerLost |
        dsNoParamSets |
        dsRefListNullPtrs;

    if ((state_bits & reference_errors) != 0) {
        m_context->needs_keyframe = true;
    }

    /*
     * 部分输入只包含SPS/PPS，或者解码器还在等待完整帧。
     * 这种情况不一定是错误，但暂时没有I420帧可输出。
     */
    if (buffer_info.iBufferStatus != 1) {
        if (state != dsErrorFree && state != dsFramePending) {
            m_context->needs_keyframe = true;
        }

        return false;
    }

    if (!planes[0] || !planes[1] || !planes[2]) {
        LOG_ERROR("OpenH264 returned null I420 planes");
        return false;
    }

    const SSysMEMBuffer& system_buffer =
        buffer_info.UsrData.sSystemBuffer;

    const int width = system_buffer.iWidth;
    const int height = system_buffer.iHeight;
    const int y_stride = system_buffer.iStride[0];
    const int uv_stride = system_buffer.iStride[1];

    if (system_buffer.iFormat != videoFormatI420 ||
        width <= 0 || height <= 0 ||
        (width % 2) != 0 || (height % 2) != 0 ||
        y_stride < width || uv_stride < width / 2) {
        LOG_ERROR("OpenH264 returned an invalid I420 frame");
        return false;
    }

    const int uv_width = width / 2;
    const int uv_height = height / 2;

    const std::size_t y_size =
        static_cast<std::size_t>(width) * height;

    const std::size_t uv_size =
        static_cast<std::size_t>(uv_width) * uv_height;

    /*
     * OpenH264内部解码缓冲区会被下一次Decode覆盖。
     * 输出帧必须拥有独立内存，才能安全交给异步渲染线程。
     *
     * 公共I420Frame使用紧密排列数据，不保存stride。
     */
    auto frame = std::make_shared<RawVideoFrame>();

    frame->data[0] = std::shared_ptr<std::uint8_t[]>(
        new (std::nothrow) std::uint8_t[y_size]);

    frame->data[1] = std::shared_ptr<std::uint8_t[]>(
        new (std::nothrow) std::uint8_t[uv_size]);

    frame->data[2] = std::shared_ptr<std::uint8_t[]>(
        new (std::nothrow) std::uint8_t[uv_size]);

    if (!frame->data[0] || !frame->data[1] || !frame->data[2]) {
        LOG_ERROR("Failed to allocate decoded I420 frame");
        return false;
    }

    /*
     * OpenH264每一行末尾可能包含对齐填充，因此不能直接按照
     * stride * height整体复制，必须逐行去掉padding。
     */
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            frame->data[0].get() +
                static_cast<std::size_t>(row) * width,
            planes[0] +
                static_cast<std::size_t>(row) * y_stride,
            static_cast<std::size_t>(width));
    }

    for (int row = 0; row < uv_height; ++row) {
        std::memcpy(
            frame->data[1].get() +
                static_cast<std::size_t>(row) * uv_width,
            planes[1] +
                static_cast<std::size_t>(row) * uv_stride,
            static_cast<std::size_t>(uv_width));

        std::memcpy(
            frame->data[2].get() +
                static_cast<std::size_t>(row) * uv_width,
            planes[2] +
                static_cast<std::size_t>(row) * uv_stride,
            static_cast<std::size_t>(uv_width));
    }

    frame->width = width;
    frame->height = height;
    frame->timestamp_us = input.timestamp_us;

    output = std::move(frame);

    /*
     * 成功解码IDR后，参考帧链已经重新建立。
     */
    if (input.is_keyframe &&
        (state_bits & reference_errors) == 0) {
        m_context->needs_keyframe = false;
    }

    return true;
}

bool H264Decoder::NeedsKeyframe() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_context && m_context->needs_keyframe;
}

void H264Decoder::ClearKeyframeRequest() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_context) {
        m_context->needs_keyframe = false;
    }
}

} // namespace CODEC
} // namespace TRANSPORT