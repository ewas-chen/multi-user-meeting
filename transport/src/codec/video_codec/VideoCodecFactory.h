#pragma once

#include "VideoCodecInterface.h"

#include <memory>


namespace TRANSPORT {
namespace CODEC {

/**
 * @brief 视频编解码器工厂
 *
 * 工厂只负责创建指定类型的编码器或解码器。
 * 对象创建完成后，由调用方负责执行Initialize()。
 */
class VideoCodecFactory final {
public:
    VideoCodecFactory() = delete;
    ~VideoCodecFactory() = delete;

    VideoCodecFactory(const VideoCodecFactory&) = delete;
    VideoCodecFactory& operator=(const VideoCodecFactory&) = delete;
    VideoCodecFactory(VideoCodecFactory&&) = delete;
    VideoCodecFactory& operator=(VideoCodecFactory&&) = delete;

    /**
     * @brief 创建指定类型的视频编码器
     *
     * 不支持指定类型时返回nullptr。
     */
    [[nodiscard]]
    static std::unique_ptr<IVideoEncoder> CreateEncoder(VideoCodecType type);

    /**
     * @brief 创建指定类型的视频解码器
     *
     * 不支持指定类型时返回nullptr。
     */
    [[nodiscard]]
    static std::unique_ptr<IVideoDecoder> CreateDecoder(VideoCodecType type);
};

} // namespace CODEC
} // namespace TRANSPORT