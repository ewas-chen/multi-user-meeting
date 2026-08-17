#pragma once

#include "VideoCodecInterface.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace TRANSPORT {
namespace CODEC {

/**
 * @brief 基于OpenH264的视频编码器
 *
 * 输入公共I420Frame，输出Annex-B格式的H.264数据。
 *
 * Encode()执行同步编码，后续应由传输模块的视频编码线程调用，
 * 不能直接在OBS视频回调线程中执行。
 */
class H264Encoder final : public IVideoEncoder {
public:
    H264Encoder();
    ~H264Encoder() override;

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;
    H264Encoder(H264Encoder&&) = delete;
    H264Encoder& operator=(H264Encoder&&) = delete;

    bool Initialize(const VideoCodecConfig& config) override;
    void Release() override;

    bool Encode(const std::shared_ptr<RawVideoFrame>& input,
                EncodedVideoFrame& output) override;

    bool SetBitrate(std::uint32_t bitrate_kbps) override;
    bool SetFramerate(std::uint32_t framerate) override;
    bool RequestKeyframe() override;

    VideoCodecType GetCodecType() const noexcept override {
        return VideoCodecType::kH264;
    }

private:
    /*
     * 隐藏OpenH264的ISVCEncoder、编码参数和临时输出缓存。
     * OpenH264头文件只需要在H264Codec.cpp中包含。
     */
    struct EncoderContext;
    std::unique_ptr<EncoderContext> m_context;

    // 保护编码器状态和动态参数修改
    mutable std::mutex m_mutex;
};

/**
 * @brief 基于OpenH264的视频解码器
 *
 * 输入Annex-B格式H.264数据，输出公共I420Frame。
 * 解码结果可以直接回调给RenderEngine。
 */
class H264Decoder final : public IVideoDecoder {
public:
    H264Decoder();
    ~H264Decoder() override;

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;
    H264Decoder(H264Decoder&&) = delete;
    H264Decoder& operator=(H264Decoder&&) = delete;

    bool Initialize(const VideoCodecConfig& config) override;
    void Release() override;

    bool Decode(const EncodedVideoFrame& input,
                std::shared_ptr<RawVideoFrame>& output) override;

    VideoCodecType GetCodecType() const noexcept override {
        return VideoCodecType::kH264;
    }

    bool NeedsKeyframe() const noexcept override;
    void ClearKeyframeRequest() noexcept override;

private:
    /*
     * 隐藏OpenH264的ISVCDecoder、解码参数和状态。
     */
    struct DecoderContext;
    std::unique_ptr<DecoderContext> m_context;

    mutable std::mutex m_mutex;
};

} // namespace CODEC
} // namespace TRANSPORT