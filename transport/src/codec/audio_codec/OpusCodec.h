#pragma once

#include "AudioCodecInterface.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace TRANSPORT {
namespace CODEC {

/**
 * @brief Opus音频编码器
 *
 * 输入：
 *   采集模块产生的交错Float32 PCM AudioFrame。
 *
 * 输出：
 *   可以交给WebRTC音频轨道发送的Opus数据包。
 *
 * 编码器内部负责：
 *   1. 必要的音频重采样。
 *   2. 将任意长度PCM整理为固定Opus帧长。
 *   3. Opus编码及编码结果缓存。
 */
class OpusCodecEncoder final : public IAudioEncoder
{
public:
    OpusCodecEncoder();
    ~OpusCodecEncoder() override;

    OpusCodecEncoder(const OpusCodecEncoder&) = delete;
    OpusCodecEncoder& operator=(const OpusCodecEncoder&) = delete;
    OpusCodecEncoder(OpusCodecEncoder&&) = delete;
    OpusCodecEncoder& operator=(OpusCodecEncoder&&) = delete;

    bool Initialize(
        const AudioCodecConfig& config) override;

    void Release() override;

    bool PushInput(
        const std::shared_ptr<RawAudioFrame>& input) override;

    bool PullEncoded(
        EncodedAudioFrame& output) override;

    std::size_t
    GetPendingFrameCount() const override;

    bool Flush() override;

    bool SetBitrate(std::uint32_t bitrate_kbps) override;

    bool SetComplexity(std::uint32_t complexity) override;

    bool SetVBR(bool enable) override;
    bool SetDTX(bool enable) override;
    bool SetFEC(bool enable) override;

    AudioCodecType GetCodecType() const noexcept override {
        return AudioCodecType::kOpus;
    }

    std::uint32_t
    GetFrameSizeSamples() const noexcept override;

private:
    /*
     * 隐藏OpusEncoder、SRC_STATE、PCM缓存和输出队列。
     * 第三方库头文件只需要在OpusCodec.cpp中包含。
     */
    struct EncoderContext;
    std::unique_ptr<EncoderContext> m_context;

    /*
     * 保护编码器状态、动态参数和输出队列。
     * 实际编码应由传输模块的音频工作线程调用。
     */
    mutable std::mutex m_mutex;
};

/**
 * @brief Opus音频解码器
 *
 * 输入：
 *   WebRTC音频轨道接收到的Opus数据包。
 *
 * 输出：
 *   可以直接交给RenderEngine的公共AudioFrame。
 *
 * 解码器内部负责：
 *   1. Opus解码。
 *   2. 必要的输出重采样。
 *   3. 丢包时生成PLC补偿音频。
 */
class OpusCodecDecoder final : public IAudioDecoder
{
public:
    OpusCodecDecoder();
    ~OpusCodecDecoder() override;

    OpusCodecDecoder(const OpusCodecDecoder&) = delete;
    OpusCodecDecoder& operator=(const OpusCodecDecoder&) = delete;
    OpusCodecDecoder(OpusCodecDecoder&&) = delete;
    OpusCodecDecoder& operator=(OpusCodecDecoder&&) = delete;

    bool Initialize(
        const AudioCodecConfig& config) override;

    void Release() override;

    bool PushInput(
        const EncodedAudioFrame& input) override;

    bool PullDecoded(
        std::shared_ptr<RawAudioFrame>& output) override;

    std::size_t
    GetPendingFrameCount() const override;

    bool Flush() override;

    bool ConcealLostPacket(
        std::shared_ptr<RawAudioFrame>& output) override;

    AudioCodecType
    GetCodecType() const noexcept override
    {
        return AudioCodecType::kOpus;
    }

    std::uint32_t
    GetFrameSizeSamples() const noexcept override;

private:
    /*
     * 隐藏OpusDecoder、SRC_STATE、解码缓存和输出队列。
     */
    struct DecoderContext;
    std::unique_ptr<DecoderContext> m_context;

    mutable std::mutex m_mutex;
};

} // namespace CODEC
} // namespace TRANSPORT