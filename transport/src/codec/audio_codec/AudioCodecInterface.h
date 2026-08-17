#pragma once

#include "TransportDefine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace TRANSPORT {
namespace CODEC {

/**
 * 音频编解码器类型
 *
 * 已有枚举值保持稳定，后续新增编解码器只能向后追加。
 */
enum class AudioCodecType : std::uint8_t
{
    kOpus = 0
};

/**
 * 原始音频采样格式
 *
 * 当前采集和渲染模块统一使用交错存储的Float32 PCM。
 */
enum class AudioSampleFormat : std::uint8_t
{
    kFloat32 = 0
};

/*
 * RawAudioFrame只是公共AudioFrame的别名，不是新的数据类型。
 * 编解码器、采集模块和渲染模块使用完全相同的帧结构。
 */
using RawAudioFrame = AudioFrame;

/**
 * 音频编解码器配置
 */
struct AudioCodecConfig
{
    // 输入或解码输出PCM的采样率
    std::uint32_t sample_rate{48000};

    // 当前Opus实现支持单声道或双声道
    std::uint32_t channels{2};

    // Opus目标码率，单位kbps
    // 编码后每秒大约传输 64 Kbit。码率越高，音质通常越好，但占用带宽越大。这里表示总码率，不是每声道码率。
    std::uint32_t bitrate_kbps{64};

    // 每个Opus包对应的音频时长
    std::uint32_t frame_size_ms{20};

    // Opus编码复杂度，范围0～10
    std::uint32_t complexity{5};

    AudioSampleFormat sample_format{AudioSampleFormat::kFloat32};

    // 可变码率
    // VBR：根据音频复杂度动态调整每个包的大小。说话复杂时使用更多数据，
    // 安静或简单时使用更少数据，通常音质和带宽利用率更好
    bool enable_vbr{true};

    // 静音时降低数据发送量
    // DTX：检测到静音时停止或极少发送音频数据，节省带宽。会议中可能需要保留少量背景音，因此默认关闭更稳妥
    bool enable_dtx{false};

    // 发送当前包时，额外附带前一帧的部分信息。如果前一包丢失而当前包到达，解码器可能恢复前一帧
    bool enable_fec{true};

    // 丢包且无法使用 FEC 时，由解码器估算并生成替代音频
    // PLC 不需要发送额外数据，但只是“猜测”，恢复效果不如真实数据
    bool enable_plc{true};
};

/**
 * Opus编码后的一帧音频
 */
struct EncodedAudioFrame
{
    std::vector<std::uint8_t> data;

    // 当前音频块第一个采样点对应的时间戳
    std::int64_t timestamp_us{0};

    // 此编码包代表的每声道采样点数量
    // 48kHz、20ms时通常为960
    std::uint32_t samples_per_channel{0};

    AudioCodecType codec_type{AudioCodecType::kOpus};

    bool IsValid() const noexcept
    {
        return !data.empty() && samples_per_channel > 0;
    }
};

/**
 * 音频编码器接口
 *
 * CaptureEngine产生的音频块不一定正好是20ms。
 * PushInput()负责接收这些音频块，编码器内部进行缓存和分帧；
 * PullEncoded()用于取出已经生成的Opus包。
 */
class IAudioEncoder
{
public:
    virtual ~IAudioEncoder() = default;

    /**
     * 初始化编码器
     */
    virtual bool Initialize(const AudioCodecConfig& config) = 0;

    /**
     * 释放编码器及内部缓存
     */
    virtual void Release() = 0;

    /**
     * 提交一块Float32 PCM音频
     *
     * input直接使用采集模块的AudioFrame。
     */
    virtual bool PushInput(
        const std::shared_ptr<RawAudioFrame>& input) = 0;

    /**
     * 取出一个已经编码完成的Opus包
     *
     * 没有完整编码帧可读时返回false。
     */
    virtual bool PullEncoded(
        EncodedAudioFrame& output) = 0;

    /**
     * 获取当前可读取的编码帧数量
     */
    virtual std::size_t GetPendingFrameCount() const = 0;

    /**
     * 处理或清理内部剩余PCM数据
     */
    virtual bool Flush() = 0;

    /**
     * 动态修改Opus目标码率
     */
    virtual bool SetBitrate(std::uint32_t bitrate_kbps) = 0;
        
    /**
     * 动态修改编码复杂度
     */
    virtual bool SetComplexity(std::uint32_t complexity) = 0;

    virtual bool SetVBR(bool enable) = 0;
    virtual bool SetDTX(bool enable) = 0;
    virtual bool SetFEC(bool enable) = 0;

    virtual AudioCodecType
    GetCodecType() const noexcept = 0;

    /**
     * 获取一个编码帧需要的每声道采样点数量
     */
    virtual std::uint32_t
    GetFrameSizeSamples() const noexcept = 0;
};

/**
 * 音频解码器接口
 *
 * 解码结果直接生成公共AudioFrame，
 * 可以直接回调给RenderEngine，不需要再次转换结构。
 */
class IAudioDecoder
{
public:
    virtual ~IAudioDecoder() = default;

    virtual bool Initialize(
        const AudioCodecConfig& config) = 0;

    virtual void Release() = 0;

    /**
     * 提交一个收到的Opus编码包
     */
    virtual bool PushInput(
        const EncodedAudioFrame& input) = 0;

    /**
     * 获取一帧解码后的Float32 PCM音频
     */
    virtual bool PullDecoded(
        std::shared_ptr<RawAudioFrame>& output) = 0;

    virtual std::size_t
    GetPendingFrameCount() const = 0;

    virtual bool Flush() = 0;

    /**
     * 在音频包丢失时生成PLC补偿音频
     */
    virtual bool ConcealLostPacket(
        std::shared_ptr<RawAudioFrame>& output) = 0;

    virtual AudioCodecType
    GetCodecType() const noexcept = 0;

    virtual std::uint32_t
    GetFrameSizeSamples() const noexcept = 0;
};

} // namespace CODEC
} // namespace TRANSPORT