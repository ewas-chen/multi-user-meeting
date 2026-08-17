#pragma once
#include <cstdint>

namespace RENDER {

// AudioRender使用的内部音频数据源接口, AudioRender的实时播放回调通过该接口从AudioMixer中读取
// 已完成混音的Float32 PCM数据
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    // 读取混音后的音频数据
    // 如果数据不足，实现应使用静音补齐剩余部分，不能让播放缓冲区保留未初始化数据
    virtual std::uint32_t PopAudio(
        float* output,
        std::uint32_t frame_count) noexcept = 0;
};

}