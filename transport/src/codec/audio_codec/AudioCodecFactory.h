#pragma once

#include "AudioCodecInterface.h"

#include <memory>

namespace TRANSPORT {
namespace CODEC {

/**
 * @brief 音频编解码器工厂
 *
 * 工厂只负责根据AudioCodecType创建对应的编码器或解码器。
 * 创建完成后，由调用方负责执行Initialize()。
 */
class AudioCodecFactory final
{
public:
    AudioCodecFactory() = delete;
    ~AudioCodecFactory() = delete;

    AudioCodecFactory(const AudioCodecFactory&) = delete;
    AudioCodecFactory& operator=(const AudioCodecFactory&) = delete;
    AudioCodecFactory(AudioCodecFactory&&) = delete;
    AudioCodecFactory& operator=(AudioCodecFactory&&) = delete;

    /**
     * @brief 创建指定类型的音频编码器
     *
     * 不支持指定类型时返回nullptr。
     */
    [[nodiscard]]
    static std::unique_ptr<IAudioEncoder>
    CreateEncoder(AudioCodecType type);

    /**
     * @brief 创建指定类型的音频解码器
     *
     * 不支持指定类型时返回nullptr。
     */
    [[nodiscard]]
    static std::unique_ptr<IAudioDecoder>
    CreateDecoder(AudioCodecType type);
};

} // namespace CODEC
} // namespace TRANSPORT