#pragma once

#include "TransportDefine.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace TRANSPORT {
namespace CODEC {

/**
 * 视频编解码器类型
 *
 * 已有枚举值保持稳定，后续新增类型只能向后追加。
 */
enum class VideoCodecType : std::uint8_t {
    kH264 = 0
};

/*
 * 编解码器直接使用采集模块的公共I420Frame。
 * 这只是类型别名，不会创建新对象或复制视频数据。
 */
using RawVideoFrame = I420Frame;

/**
 * 视频编解码器配置
 */
struct VideoCodecConfig {
    // 必须与采集模块实际输出尺寸一致
    int width{0};
    int height{0};
    int framerate{30};

    // H.264码率范围，单位kbps
    std::uint32_t target_bitrate_kbps{2000};
    std::uint32_t max_bitrate_kbps{3000};
    std::uint32_t min_bitrate_kbps{500};

    // 两个关键帧之间最多允许的普通视频帧数量
    std::uint32_t keyframe_interval{60};

    // 0表示让OpenH264自动选择编码线程数
    std::uint32_t threads{0};

    // 网络或编码性能不足时允许编码器主动跳帧
    bool enable_frame_dropping{true};
};

/**
 * H.264编码后的一帧视频
 *
 * data保存一个完整H.264 Access Unit，使用Annex-B格式：
 *
 *   00 00 00 01 + NAL
 *
 * 这种格式可以直接交给libdatachannel的H264RtpPacketizer。
 */
struct EncodedVideoFrame {
    std::vector<std::uint8_t> data;

    int width{0};
    int height{0};
    std::int64_t timestamp_us{0};

    VideoCodecType codec_type{VideoCodecType::kH264};

    // IDR关键帧可以在没有前面参考帧的情况下独立解码,P帧依赖IDR
    bool is_keyframe{false};

    // 编码器主动跳帧时为true，此时data通常为空（CPU压力、网络拥塞时），不用直接丢弃因为时间戳继续推进
    bool is_skipped{false};

    bool HasData() const noexcept {
        return !data.empty() && width > 0 && height > 0;
    }
};

/**
 * 视频编码器接口
 *
 * Encode()本身执行同步编码，因此后续必须由传输模块的视频工作线程调用，
 * 不能直接在OBS视频回调线程中执行。
 */
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual bool Initialize(const VideoCodecConfig& config) = 0;
    virtual void Release() = 0;

    /**
     * 编码一帧I420视频
     *
     * 返回true表示编码过程正常。
     * 如果编码器主动跳帧，仍然返回true，但output.is_skipped为true。
     */
    virtual bool Encode(const std::shared_ptr<RawVideoFrame>& input,
                        EncodedVideoFrame& output) = 0;

    // 运行过程中动态调整码率和帧率
    virtual bool SetBitrate(std::uint32_t bitrate_kbps) = 0;
    virtual bool SetFramerate(std::uint32_t framerate) = 0;

    // 要求下一帧编码为IDR关键帧
    virtual bool RequestKeyframe() = 0;

    virtual VideoCodecType GetCodecType() const noexcept = 0;
};

/**
 * 视频解码器接口
 *
 * 解码结果直接生成公共I420Frame，可以直接交给RenderEngine。
 */
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual bool Initialize(const VideoCodecConfig& config) = 0;
    virtual void Release() = 0;

    virtual bool Decode(const EncodedVideoFrame& input,
                        std::shared_ptr<RawVideoFrame>& output) = 0;

    virtual VideoCodecType GetCodecType() const noexcept = 0;

    /**
     * 查询解码器是否需要新的关键帧
     *
     * 收到损坏数据、参考帧丢失或解码状态无法恢复时返回true。
     * 传输层可以据此通过RTCP PLI请求发送端产生关键帧。
     */
    virtual bool NeedsKeyframe() const noexcept = 0;

    /**
     * 清除关键帧请求状态
     *
     * 通常在发送PLI或成功解码关键帧后调用。
     */
    virtual void ClearKeyframeRequest() noexcept = 0;
};

} // namespace CODEC
} // namespace TRANSPORT