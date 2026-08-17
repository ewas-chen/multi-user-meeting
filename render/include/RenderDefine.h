#pragma once

#include "ICaptureEngine.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/*
 * Linux ELF动态库符号可见性。
 * 建议配合编译选项-fvisibility=hidden使用。
 */
#define RENDER_ENGINE_API __attribute__((visibility("default")))

#define RENDER_ENGINE_LOCAL __attribute__((visibility("hidden")))

namespace RENDER {
/*
    Render模块直接复用Capture模块的音视频帧类型
    视频格式：紧凑存储的I420/YUV420P
    音频格式：交错存储的Float32 PCM
*/
using I420Frame = CAPTURE::I420Frame;
using AudioFrame = CAPTURE::AudioFrame;

// 音频播放设备信息
struct AudioSpeaker {
    std::string device_id;
    std::string name;
    bool is_default{false};

    AudioSpeaker() = default;

    AudioSpeaker(std::string id, std::string device_name, bool default_device = false) : 
    device_id(std::move(id)), name(std::move(device_name)), is_default(default_device) {}
};

// 混音器内部使用的音频块, data保存交错排列的Float32 PCM, 保存所有用户相加后的一个短音频块
struct MixedAudioChunk {
    std::int64_t start_timestamp_us{0};
    std::uint32_t frame_count{0};
    std::vector<float> data;

    MixedAudioChunk() = default;

    MixedAudioChunk(std::int64_t timestamp_us, std::uint32_t frames, std::vector<float> audio_data)
        : start_timestamp_us(timestamp_us), frame_count(frames), data(std::move(audio_data)) {}

    bool IsValid() const noexcept {
        return frame_count > 0 && !data.empty();
    }
};


} // namespace

