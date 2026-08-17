#include "transport/src/codec/audio_codec/AudioCodecFactory.h"
#include "transport/src/codec/video_codec/VideoCodecFactory.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace TRANSPORT::CODEC;

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioFrameSamples = 960;  // 48kHz下20ms音频

constexpr int kVideoWidth = 640;
constexpr int kVideoHeight = 480;
constexpr int kVideoFps = 30;

constexpr const char* kAudioInputFile =
    "/home/ewas/VC_code/meeting-client/source/microphone_48000hz_2ch_f32le.pcm";

constexpr const char* kVideoInputFile =
    "/home/ewas/VC_code/meeting-client/source/camera_640x480_i420.yuv";

constexpr const char* kAudioOutputFile =
    "codec_decoded_48000hz_2ch_f32le.pcm";

constexpr const char* kVideoOutputFile =
    "codec_decoded_640x480_i420.yuv";

struct AudioTestResult {
    std::size_t input_frames{0};
    std::size_t encoded_packets{0};
    std::size_t decoded_frames{0};
    std::size_t decoded_samples{0};
    std::size_t invalid_frames{0};
};

struct VideoTestResult {
    std::size_t input_frames{0};
    std::size_t encoded_frames{0};
    std::size_t skipped_frames{0};
    std::size_t decoded_frames{0};
    std::size_t invalid_frames{0};
};

/**
 * @brief 从PCM文件读取一个20ms音频帧
 *
 * 输入文件必须是交错排列的Float32 PCM：
 * L R L R ...（双声道时）。
 */
std::shared_ptr<RawAudioFrame> ReadAudioFrame(
    std::ifstream& input,
    std::int64_t timestamp_us)
{
    const std::size_t data_size =
        static_cast<std::size_t>(kAudioFrameSamples) *
        kAudioChannels * sizeof(float);

    auto frame = std::make_shared<RawAudioFrame>();
    frame->data = std::shared_ptr<std::uint8_t[]>(
        new std::uint8_t[data_size]);

    input.read(
        reinterpret_cast<char*>(frame->data.get()),
        static_cast<std::streamsize>(data_size));

    if (input.gcount() != static_cast<std::streamsize>(data_size)) {
        return nullptr;
    }

    frame->samples = kAudioFrameSamples;
    frame->sample_rate = kAudioSampleRate;
    frame->channels = kAudioChannels;
    frame->timestamp_us = timestamp_us;

    return frame;
}

/**
 * @brief 检查解码后的PCM是否有效
 *
 * 除了检查帧结构，还检查PCM中是否出现NaN或无穷大。
 */
bool ValidateDecodedAudioFrame(
    const std::shared_ptr<RawAudioFrame>& frame)
{
    if (!frame || !frame->IsValid()) {
        return false;
    }

    if (frame->sample_rate != kAudioSampleRate ||
        frame->channels != kAudioChannels) {
        return false;
    }

    const std::size_t sample_count =
        static_cast<std::size_t>(frame->samples) *
        frame->channels;

    const float* samples =
        reinterpret_cast<const float*>(frame->data.get());

    for (std::size_t index = 0; index < sample_count; ++index) {
        if (!std::isfinite(samples[index])) {
            return false;
        }
    }

    return true;
}

/**
 * @brief 测试Opus编码和解码闭环
 *
 * 流程：
 * PCM文件 -> Opus编码器 -> Opus数据包 ->
 * Opus解码器 -> PCM文件。
 */
bool TestAudioCodec(AudioTestResult& result)
{
    std::cout << "\n========== Opus编解码测试 ==========\n";

    std::ifstream input(kAudioInputFile, std::ios::binary);
    if (!input) {
        std::cerr << "无法打开音频输入文件: "
                  << kAudioInputFile << '\n';
        return false;
    }

    std::ofstream output(kAudioOutputFile, std::ios::binary);
    if (!output) {
        std::cerr << "无法创建音频输出文件: "
                  << kAudioOutputFile << '\n';
        return false;
    }

    AudioCodecConfig config;
    config.sample_rate = kAudioSampleRate;
    config.channels = kAudioChannels;
    config.bitrate_kbps = 64;
    config.frame_size_ms = 20;
    config.complexity = 5;
    config.sample_format = AudioSampleFormat::kFloat32;
    config.enable_vbr = true;
    config.enable_dtx = false;
    config.enable_fec = true;
    config.enable_plc = true;

    auto encoder =
        AudioCodecFactory::CreateEncoder(AudioCodecType::kOpus);

    auto decoder =
        AudioCodecFactory::CreateDecoder(AudioCodecType::kOpus);

    if (!encoder || !decoder) {
        std::cerr << "创建Opus编解码器失败\n";
        return false;
    }

    if (!encoder->Initialize(config)) {
        std::cerr << "初始化Opus编码器失败\n";
        return false;
    }

    if (!decoder->Initialize(config)) {
        std::cerr << "初始化Opus解码器失败\n";
        encoder->Release();
        return false;
    }

    std::int64_t last_decoded_timestamp = -1;

    while (true) {
        const std::int64_t timestamp_us =
            static_cast<std::int64_t>(result.input_frames) *
            kAudioFrameSamples * 1000000LL /
            kAudioSampleRate;

        auto input_frame = ReadAudioFrame(input, timestamp_us);
        if (!input_frame) {
            break;
        }

        ++result.input_frames;

        if (!encoder->PushInput(input_frame)) {
            std::cerr << "音频帧送入编码器失败，帧号: "
                      << result.input_frames << '\n';
            ++result.invalid_frames;
            continue;
        }

        EncodedAudioFrame encoded_frame;

        while (encoder->PullEncoded(encoded_frame)) {
            if (encoded_frame.data.empty()) {
                ++result.invalid_frames;
                continue;
            }

            ++result.encoded_packets;

            if (!decoder->PushInput(encoded_frame)) {
                std::cerr << "Opus数据送入解码器失败，包号: "
                          << result.encoded_packets << '\n';
                ++result.invalid_frames;
                continue;
            }

            std::shared_ptr<RawAudioFrame> decoded_frame;

            while (decoder->PullDecoded(decoded_frame)) {
                if (!ValidateDecodedAudioFrame(decoded_frame)) {
                    ++result.invalid_frames;
                    continue;
                }

                if (last_decoded_timestamp >= 0 &&
                    decoded_frame->timestamp_us <
                    last_decoded_timestamp) {
                    std::cerr << "音频时间戳发生倒退\n";
                    ++result.invalid_frames;
                }

                last_decoded_timestamp =
                    decoded_frame->timestamp_us;

                const std::size_t data_size =
                    static_cast<std::size_t>(
                        decoded_frame->samples) *
                    decoded_frame->channels *
                    sizeof(float);

                output.write(
                    reinterpret_cast<const char*>(
                        decoded_frame->data.get()),
                    static_cast<std::streamsize>(data_size));

                ++result.decoded_frames;
                result.decoded_samples +=
                    decoded_frame->samples;
            }
        }
    }

    encoder->Flush();

    EncodedAudioFrame encoded_frame;
    while (encoder->PullEncoded(encoded_frame)) {
        if (encoded_frame.data.empty()) {
            ++result.invalid_frames;
            continue;
        }

        ++result.encoded_packets;

        if (!decoder->PushInput(encoded_frame)) {
            ++result.invalid_frames;
            continue;
        }

        std::shared_ptr<RawAudioFrame> decoded_frame;

        while (decoder->PullDecoded(decoded_frame)) {
            if (!ValidateDecodedAudioFrame(decoded_frame)) {
                ++result.invalid_frames;
                continue;
            }

            const std::size_t data_size =
                static_cast<std::size_t>(
                    decoded_frame->samples) *
                decoded_frame->channels *
                sizeof(float);

            output.write(
                reinterpret_cast<const char*>(
                    decoded_frame->data.get()),
                static_cast<std::streamsize>(data_size));

            ++result.decoded_frames;
            result.decoded_samples += decoded_frame->samples;
        }
    }

    decoder->Flush();
    encoder->Release();
    decoder->Release();

    const bool passed =
        result.input_frames > 0 &&
        result.encoded_packets > 0 &&
        result.decoded_frames > 0 &&
        result.invalid_frames == 0 &&
        output.good();

    std::cout << "输入PCM帧数: " << result.input_frames << '\n';
    std::cout << "Opus包数量: " << result.encoded_packets << '\n';
    std::cout << "解码PCM帧数: " << result.decoded_frames << '\n';
    std::cout << "解码采样数（每声道）: "
              << result.decoded_samples << '\n';
    std::cout << "无效音频帧数: " << result.invalid_frames << '\n';
    std::cout << "解码输出文件: " << kAudioOutputFile << '\n';
    std::cout << "Opus测试结果: "
              << (passed ? "通过" : "失败") << '\n';

    return passed;
}

/**
 * @brief 从I420文件读取一帧视频
 *
 * 文件排列顺序为：
 * Y平面 -> U平面 -> V平面。
 */
std::shared_ptr<RawVideoFrame> ReadVideoFrame(
    std::ifstream& input,
    std::int64_t timestamp_us)
{
    const std::size_t y_size =
        static_cast<std::size_t>(kVideoWidth) *
        kVideoHeight;

    const std::size_t uv_size =
        static_cast<std::size_t>(kVideoWidth / 2) *
        (kVideoHeight / 2);

    auto frame = std::make_shared<RawVideoFrame>();
    frame->data[0] = std::shared_ptr<std::uint8_t[]>(
        new std::uint8_t[y_size]);
    frame->data[1] = std::shared_ptr<std::uint8_t[]>(
        new std::uint8_t[uv_size]);
    frame->data[2] = std::shared_ptr<std::uint8_t[]>(
        new std::uint8_t[uv_size]);

    input.read(
        reinterpret_cast<char*>(frame->data[0].get()),
        static_cast<std::streamsize>(y_size));

    input.read(
        reinterpret_cast<char*>(frame->data[1].get()),
        static_cast<std::streamsize>(uv_size));

    input.read(
        reinterpret_cast<char*>(frame->data[2].get()),
        static_cast<std::streamsize>(uv_size));

    if (input.gcount() != static_cast<std::streamsize>(uv_size)) {
        return nullptr;
    }

    frame->width = kVideoWidth;
    frame->height = kVideoHeight;
    frame->timestamp_us = timestamp_us;

    return frame;
}

/**
 * @brief 将解码后的I420视频帧写入本地文件
 */
bool WriteDecodedVideoFrame(
    std::ofstream& output,
    const std::shared_ptr<RawVideoFrame>& frame)
{
    if (!frame || !frame->IsValid() ||
        frame->width != kVideoWidth ||
        frame->height != kVideoHeight) {
        return false;
    }

    const std::size_t y_size =
        static_cast<std::size_t>(frame->width) *
        frame->height;

    const std::size_t uv_size =
        static_cast<std::size_t>(frame->width / 2) *
        (frame->height / 2);

    output.write(
        reinterpret_cast<const char*>(frame->data[0].get()),
        static_cast<std::streamsize>(y_size));

    output.write(
        reinterpret_cast<const char*>(frame->data[1].get()),
        static_cast<std::streamsize>(uv_size));

    output.write(
        reinterpret_cast<const char*>(frame->data[2].get()),
        static_cast<std::streamsize>(uv_size));

    return output.good();
}

/**
 * @brief 测试H264编码和解码闭环
 *
 * 流程：
 * I420文件 -> OpenH264编码器 -> Annex-B H264 ->
 * OpenH264解码器 -> I420文件。
 */
bool TestVideoCodec(VideoTestResult& result)
{
    std::cout << "\n========== H264编解码测试 ==========\n";

    std::ifstream input(kVideoInputFile, std::ios::binary);
    if (!input) {
        std::cerr << "无法打开视频输入文件: "
                  << kVideoInputFile << '\n';
        return false;
    }

    std::ofstream output(kVideoOutputFile, std::ios::binary);
    if (!output) {
        std::cerr << "无法创建视频输出文件: "
                  << kVideoOutputFile << '\n';
        return false;
    }

    VideoCodecConfig config;
    config.width = kVideoWidth;
    config.height = kVideoHeight;
    config.framerate = kVideoFps;
    config.target_bitrate_kbps = 1200;
    config.max_bitrate_kbps = 2000;
    config.min_bitrate_kbps = 300;
    config.keyframe_interval = kVideoFps * 2;
    config.enable_frame_dropping = true;

    auto encoder =
        VideoCodecFactory::CreateEncoder(VideoCodecType::kH264);

    auto decoder =
        VideoCodecFactory::CreateDecoder(VideoCodecType::kH264);

    if (!encoder || !decoder) {
        std::cerr << "创建H264编解码器失败\n";
        return false;
    }

    if (!encoder->Initialize(config)) {
        std::cerr << "初始化H264编码器失败\n";
        return false;
    }

    if (!decoder->Initialize(config)) {
        std::cerr << "初始化H264解码器失败\n";
        encoder->Release();
        return false;
    }

    while (true) {
        const std::int64_t timestamp_us =
            static_cast<std::int64_t>(result.input_frames) *
            1000000LL / kVideoFps;

        auto input_frame = ReadVideoFrame(input, timestamp_us);
        if (!input_frame) {
            break;
        }

        ++result.input_frames;

        /*
         * 在第60帧主动请求一次关键帧，
         * 同时测试动态关键帧请求接口。
         */
        if (result.input_frames == 60 &&
            !encoder->RequestKeyframe()) {
            std::cerr << "请求H264关键帧失败\n";
            ++result.invalid_frames;
        }

        EncodedVideoFrame encoded_frame;

        if (!encoder->Encode(input_frame, encoded_frame)) {
            std::cerr << "H264编码失败，帧号: "
                      << result.input_frames << '\n';
            ++result.invalid_frames;
            continue;
        }

        if (encoded_frame.is_skipped) {
            ++result.skipped_frames;
            continue;
        }

        if (encoded_frame.data.empty()) {
            ++result.invalid_frames;
            continue;
        }

        ++result.encoded_frames;

        std::shared_ptr<RawVideoFrame> decoded_frame;

        if (!decoder->Decode(encoded_frame, decoded_frame)) {
            std::cerr << "H264解码失败，帧号: "
                      << result.input_frames << '\n';
            ++result.invalid_frames;
            continue;
        }

        /*
         * 解码器可能成功接收数据，但当前调用暂时没有输出帧。
         */
        if (!decoded_frame) {
            continue;
        }

        if (!WriteDecodedVideoFrame(output, decoded_frame)) {
            ++result.invalid_frames;
            continue;
        }

        if (decoded_frame->timestamp_us !=
            encoded_frame.timestamp_us) {
            std::cerr << "视频时间戳不一致，帧号: "
                      << result.input_frames << '\n';
            ++result.invalid_frames;
        }

        ++result.decoded_frames;
    }

    encoder->Release();
    decoder->Release();

    const bool passed =
        result.input_frames > 0 &&
        result.encoded_frames > 0 &&
        result.decoded_frames > 0 &&
        result.invalid_frames == 0 &&
        output.good();

    std::cout << "输入I420帧数: " << result.input_frames << '\n';
    std::cout << "H264编码帧数: " << result.encoded_frames << '\n';
    std::cout << "编码器跳过帧数: " << result.skipped_frames << '\n';
    std::cout << "H264解码帧数: " << result.decoded_frames << '\n';
    std::cout << "无效视频帧数: " << result.invalid_frames << '\n';
    std::cout << "解码输出文件: " << kVideoOutputFile << '\n';
    std::cout << "H264测试结果: "
              << (passed ? "通过" : "失败") << '\n';

    return passed;
}

} // namespace

int main()
{
    std::cout << "========== 本地音视频编解码测试 ==========\n";
    std::cout << "音频输入: " << kAudioInputFile << '\n';
    std::cout << "视频输入: " << kVideoInputFile << '\n';
    std::cout << "本测试不连接RTC服务器\n";

    AudioTestResult audio_result;
    VideoTestResult video_result;

    const bool audio_passed = TestAudioCodec(audio_result);
    const bool video_passed = TestVideoCodec(video_result);
    const bool all_passed = audio_passed && video_passed;

    std::cout << "\n========== 编解码测试总结 ==========\n";
    std::cout << "Opus编解码: "
              << (audio_passed ? "通过" : "失败") << '\n';
    std::cout << "H264编解码: "
              << (video_passed ? "通过" : "失败") << '\n';
    std::cout << "最终结果: "
              << (all_passed ? "通过" : "失败") << '\n';

    return all_passed ? 0 : 1;
}