#include "AdaptiveJitterController.h"
#include "AudioMixer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSampleRate = 1000;
constexpr int kChannels = 1;
constexpr std::uint32_t kChunkFrames = 10;
constexpr std::int64_t kBaseTimestampUs = 1'000'000;
constexpr std::int64_t kDefaultTargetBufferUs = 120'000;
constexpr std::int64_t kMaximumTargetBufferUs = 250'000;
constexpr float kFloatTolerance = 0.0001F;

using namespace std::chrono_literals;

struct TestStatistics {
    int passed{0};
    int failed{0};
};

/**
 * @brief 创建固定采样值的Float32 PCM音频
 *
 * 测试使用1000Hz单声道，1个采样帧对应1ms，
 * 便于直接核对媒体时间和播放时钟。
 */
std::shared_ptr<RENDER::AudioFrame> CreateConstantFrame(
    int frame_count,
    float sample_value,
    std::int64_t timestamp_us,
    int sample_rate = kSampleRate,
    int channels = kChannels)
{
    if (frame_count <= 0 || sample_rate <= 0 || channels <= 0) {
        return nullptr;
    }

    const std::size_t sample_count =
        static_cast<std::size_t>(frame_count) *
        static_cast<std::size_t>(channels);

    const std::size_t byte_count = sample_count * sizeof(float);

    auto frame = std::make_shared<RENDER::AudioFrame>();
    frame->data = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[byte_count]);

    auto* samples = reinterpret_cast<float*>(frame->data.get());
    std::fill(samples, samples + sample_count, sample_value);

    frame->samples = frame_count;
    frame->channels = channels;
    frame->sample_rate = sample_rate;
    frame->timestamp_us = timestamp_us;

    return frame;
}

bool IsNear(float actual, float expected)
{
    return std::abs(actual - expected) <= kFloatTolerance;
}

bool IsConstantChunk(
    const std::vector<float>& samples,
    float expected_value,
    std::size_t* mismatch_index = nullptr)
{
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (!IsNear(samples[index], expected_value)) {
            if (mismatch_index) {
                *mismatch_index = index;
            }
            return false;
        }
    }

    return true;
}

bool ExpectInteger(
    std::int64_t actual,
    std::int64_t expected,
    const std::string& check_name)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "  检查失败: " << check_name
              << ", expected=" << expected
              << ", actual=" << actual << '\n';

    return false;
}

void PrintClock(const RENDER::AudioMixer& mixer)
{
    const auto timestamp_us = mixer.GetPlaybackTimestampUs();

    if (timestamp_us) {
        std::cout << "  当前播放时钟: " << *timestamp_us << " us\n";
    } else {
        std::cout << "  当前播放时钟: 未建立\n";
    }
}

bool InitializeMixer(RENDER::AudioMixer& mixer)
{
    if (mixer.Initialize(kSampleRate, kChannels)) {
        return true;
    }

    std::cerr << "  AudioMixer初始化失败\n";
    return false;
}

bool PushFrame(
    RENDER::AudioMixer& mixer,
    const std::string& user_name,
    const std::shared_ptr<RENDER::AudioFrame>& frame)
{
    if (mixer.PushAudioData(user_name, frame)) {
        return true;
    }

    std::cerr << "  推送音频失败: user=" << user_name << '\n';
    return false;
}

/**
 * @brief 从AudioMixer读取指定数量的采样帧
 *
 * 后台线程需要少量时间生成预混音块，因此正常读取前短暂让出执行时间。
 */
bool PopFrames(
    RENDER::AudioMixer& mixer,
    std::uint32_t frame_count,
    std::vector<float>& output,
    bool wait_for_worker = true)
{
    if (wait_for_worker) {
        std::this_thread::sleep_for(2ms);
    }

    output.assign(
        static_cast<std::size_t>(frame_count) *
            static_cast<std::size_t>(kChannels),
        0.0F);

    const std::uint32_t popped_frames =
        mixer.PopAudio(output.data(), frame_count);

    if (popped_frames != frame_count) {
        std::cerr << "  PopAudio返回帧数错误: expected=" << frame_count
                  << ", actual=" << popped_frames << '\n';
        return false;
    }

    return true;
}

bool ExpectNextFrames(
    RENDER::AudioMixer& mixer,
    std::uint32_t frame_count,
    float expected_value,
    const std::string& check_name)
{
    std::vector<float> output;

    if (!PopFrames(mixer, frame_count, output)) {
        std::cerr << "  检查失败: " << check_name << '\n';
        return false;
    }

    std::size_t mismatch_index = 0;

    if (!IsConstantChunk(output, expected_value, &mismatch_index)) {
        std::cerr << "  检查失败: " << check_name
                  << "\n  样本位置: " << mismatch_index
                  << "\n  期望值: " << expected_value
                  << "\n  实际值: " << output[mismatch_index] << '\n';

        PrintClock(mixer);
        return false;
    }

    return true;
}

bool ExpectNextChunk(
    RENDER::AudioMixer& mixer,
    float expected_value,
    const std::string& check_name)
{
    return ExpectNextFrames(
        mixer, kChunkFrames, expected_value, check_name);
}

/**
 * @brief 等待第一块有效混音数据
 *
 * 时间线建立前的空读取不会创建播放时钟，因此可以安全轮询。
 */
bool WaitForInitialChunk(
    RENDER::AudioMixer& mixer,
    float expected_value,
    const std::string& check_name)
{
    const auto deadline = std::chrono::steady_clock::now() + 500ms;

    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<float> output;

        if (!PopFrames(mixer, kChunkFrames, output, false)) {
            return false;
        }

        std::size_t mismatch_index = 0;

        if (IsConstantChunk(output, expected_value, &mismatch_index)) {
            return true;
        }

        /*
         * 时钟有效说明已经消费了带时间戳的混音块。
         * 此时数据错误属于真实失败，不能继续跳过。
         */
        if (mixer.GetPlaybackTimestampUs()) {
            std::cerr << "  检查失败: " << check_name
                      << "\n  样本位置: " << mismatch_index
                      << "\n  期望值: " << expected_value
                      << "\n  实际值: " << output[mismatch_index] << '\n';

            PrintClock(mixer);
            return false;
        }

        std::this_thread::sleep_for(2ms);
    }

    std::cerr << "  等待混音数据超时: " << check_name << '\n';
    return false;
}

bool ExpectClock(
    const RENDER::AudioMixer& mixer,
    std::optional<std::int64_t> expected_timestamp_us,
    const std::string& check_name)
{
    const auto actual_timestamp_us = mixer.GetPlaybackTimestampUs();

    if (actual_timestamp_us == expected_timestamp_us) {
        return true;
    }

    std::cerr << "  播放时钟检查失败: " << check_name << '\n';

    if (expected_timestamp_us) {
        std::cerr << "  期望时钟: " << *expected_timestamp_us << " us\n";
    } else {
        std::cerr << "  期望时钟: 未建立\n";
    }

    PrintClock(mixer);
    return false;
}

/**
 * @brief 验证控制器默认参数和初始状态
 */
bool TestAdaptiveControllerInitialState()
{
    RENDER::AdaptiveJitterController controller;

    const auto config = controller.GetConfig();
    const auto statistics = controller.GetStatistics();

    if (config.min_target_buffer_us != 60'000 ||
        config.initial_target_buffer_us != kDefaultTargetBufferUs ||
        config.max_target_buffer_us != kMaximumTargetBufferUs ||
        config.safety_margin_us != 20'000 ||
        config.jitter_multiplier != 4) {
        std::cerr << "  AdaptiveJitterController默认配置不符合预期\n";
        return false;
    }

    return ExpectInteger(
               controller.GetTargetBufferDurationUs(),
               kDefaultTargetBufferUs,
               "初始目标缓存") &&
           ExpectInteger(
               statistics.estimated_jitter_us,
               0,
               "初始抖动估算") &&
           ExpectInteger(
               static_cast<std::int64_t>(
                   statistics.arrival_sample_count),
               0,
               "初始有效样本数");
}

/**
 * @brief 稳定20ms到达间隔不应增加默认120ms目标
 */
bool TestStableArrivalKeepsTarget()
{
    RENDER::AdaptiveJitterController controller;

    std::int64_t media_timestamp_us = 1'000'000;
    std::int64_t arrival_timestamp_us = 10'000'000;

    controller.ObserveArrival(
        media_timestamp_us, arrival_timestamp_us);

    for (int index = 0; index < 100; ++index) {
        media_timestamp_us += 20'000;
        arrival_timestamp_us += 20'000;

        controller.ObserveArrival(
            media_timestamp_us, arrival_timestamp_us);
    }

    const auto statistics = controller.GetStatistics();

    return ExpectInteger(
               controller.GetTargetBufferDurationUs(),
               kDefaultTargetBufferUs,
               "稳定输入保持默认目标") &&
           ExpectInteger(
               statistics.estimated_jitter_us,
               0,
               "稳定输入抖动估算") &&
           ExpectInteger(
               static_cast<std::int64_t>(
                   statistics.arrival_sample_count),
               100,
               "稳定输入有效样本数");
}

/**
 * @brief 持续到达波动应提高目标，但不能超过250ms上限
 */
bool TestArrivalJitterIncreasesTarget()
{
    RENDER::AdaptiveJitterController controller;

    std::int64_t media_timestamp_us = 1'000'000;
    std::int64_t arrival_timestamp_us = 10'000'000;

    controller.ObserveArrival(
        media_timestamp_us, arrival_timestamp_us);

    /*
     * 媒体时间固定每20ms推进，实际到达间隔在10ms和70ms之间变化，
     * 产生可重复且不依赖真实线程调度的抖动样本。
     */
    for (int index = 0; index < 80; ++index) {
        media_timestamp_us += 20'000;
        arrival_timestamp_us +=
            index % 2 == 0 ? 70'000 : 10'000;

        controller.ObserveArrival(
            media_timestamp_us, arrival_timestamp_us);
    }

    const auto statistics = controller.GetStatistics();
    const std::int64_t target_us =
        controller.GetTargetBufferDurationUs();

    if (statistics.estimated_jitter_us <= 0) {
        std::cerr << "  到达波动后没有建立有效抖动估算\n";
        return false;
    }

    if (target_us <= kDefaultTargetBufferUs ||
        target_us > kMaximumTargetBufferUs) {
        std::cerr << "  动态目标缓存超出预期范围: "
                  << target_us << " us\n";
        return false;
    }

    return true;
}

/**
 * @brief Reset后恢复120ms目标并清除当前抖动估算
 */
bool TestAdaptiveControllerReset()
{
    RENDER::AdaptiveJitterController controller;

    std::int64_t media_timestamp_us = 1'000'000;
    std::int64_t arrival_timestamp_us = 10'000'000;

    controller.ObserveArrival(
        media_timestamp_us, arrival_timestamp_us);

    for (int index = 0; index < 40; ++index) {
        media_timestamp_us += 20'000;
        arrival_timestamp_us += 70'000;

        controller.ObserveArrival(
            media_timestamp_us, arrival_timestamp_us);
    }

    if (controller.GetTargetBufferDurationUs() <=
        kDefaultTargetBufferUs) {
        std::cerr << "  Reset测试未先建立增大的目标缓存\n";
        return false;
    }

    controller.Reset();

    const auto statistics = controller.GetStatistics();

    return ExpectInteger(
               controller.GetTargetBufferDurationUs(),
               kDefaultTargetBufferUs,
               "Reset后恢复初始目标") &&
           ExpectInteger(
               statistics.estimated_jitter_us,
               0,
               "Reset后清除抖动估算") &&
           ExpectInteger(
               statistics.latest_jitter_sample_us,
               0,
               "Reset后清除最近样本") &&
           ExpectInteger(
               static_cast<std::int64_t>(
                   statistics.reset_count),
               1,
               "Reset计数");
}

/**
 * @brief 测试120ms启动缓存和短时欠载静音
 */
bool TestStartupBufferAndUnderrun()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    const auto first_part = CreateConstantFrame(
        110, 0.25F, kBaseTimestampUs);

    if (!PushFrame(mixer, "user_a", first_part)) {
        return false;
    }

    std::this_thread::sleep_for(20ms);

    std::vector<float> output;

    if (!PopFrames(mixer, kChunkFrames, output, false)) {
        return false;
    }

    if (!IsConstantChunk(output, 0.0F)) {
        std::cerr << "  启动缓存不足120ms时没有输出静音\n";
        return false;
    }

    if (!ExpectClock(
            mixer,
            std::nullopt,
            "启动缓存不足时不应建立播放时钟")) {
        return false;
    }

    const auto second_part = CreateConstantFrame(
        10, 0.25F, kBaseTimestampUs + 110'000);

    if (!PushFrame(mixer, "user_a", second_part)) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.25F,
            "达到120ms启动缓存后输出音频")) {
        return false;
    }

    /*
     * 第一块已经消费10ms，继续消费剩余110ms有效音频。
     */
    for (int index = 0; index < 11; ++index) {
        if (!ExpectNextChunk(
                mixer,
                0.25F,
                "启动缓存中的有效音频")) {
            return false;
        }
    }

    if (!ExpectNextChunk(
            mixer,
            0.0F,
            "有效音频耗尽后的短时静音")) {
        return false;
    }

    return ExpectClock(
        mixer,
        kBaseTimestampUs + 130'000,
        "短时静音期间播放时钟继续推进");
}

/**
 * @brief 测试多用户按照时间戳对齐混音
 */
bool TestMultiUserTimestampAlignment()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                250, 0.20F, kBaseTimestampUs))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.20F,
            "user_a建立播放时间线")) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_b",
            CreateConstantFrame(
                120,
                0.30F,
                kBaseTimestampUs + 100'000))) {
        return false;
    }

    /*
     * 当前已经播放到10ms，再消费9块到100ms。
     */
    for (int index = 0; index < 9; ++index) {
        if (!ExpectNextChunk(
                mixer,
                0.20F,
                "user_b时间戳之前只播放user_a")) {
            return false;
        }
    }

    if (!ExpectNextChunk(
            mixer,
            0.50F,
            "到达100ms后混合user_a和user_b")) {
        return false;
    }

    return ExpectClock(
        mixer,
        kBaseTimestampUs + 110'000,
        "多用户混音后的播放时钟");
}

/**
 * @brief 测试较小时间间隙自动补静音
 */
bool TestSmallGapFilledWithSilence()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                30, 0.25F, kBaseTimestampUs))) {
        return false;
    }

    /*
     * 第一段覆盖0～30ms，第二段覆盖40～130ms，
     * 总缓冲达到120ms以上，同时保留10ms间隙。
     */
    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                90,
                0.25F,
                kBaseTimestampUs + 40'000))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.25F,
            "间隙测试第一块音频")) {
        return false;
    }

    if (!ExpectNextChunk(
            mixer,
            0.25F,
            "间隙前第二块音频") ||
        !ExpectNextChunk(
            mixer,
            0.25F,
            "间隙前第三块音频")) {
        return false;
    }

    if (!ExpectNextChunk(
            mixer,
            0.0F,
            "30ms到40ms间隙补静音")) {
        return false;
    }

    return ExpectNextChunk(
        mixer,
        0.25F,
        "40ms后恢复有效音频");
}

/**
 * @brief 测试完全过期的音频不会重新进入播放队列
 */
bool TestLateFrameDropped()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                120, 0.20F, kBaseTimestampUs))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.20F,
            "过期帧测试建立播放时间线")) {
        return false;
    }

    const auto late_frame = CreateConstantFrame(
        10, 0.90F, kBaseTimestampUs + 20'000);

    if (!PushFrame(mixer, "user_a", late_frame)) {
        return false;
    }

    for (int index = 0; index < 4; ++index) {
        if (!ExpectNextChunk(
                mixer,
                0.20F,
                "过期音频不能覆盖正常播放数据")) {
            return false;
        }
    }

    return true;
}

/**
 * @brief 测试时间戳大幅跳变后通过原有欠载恢复重新建立时间线
 */
bool TestTimestampDiscontinuityRecovery()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                120, 0.20F, kBaseTimestampUs))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.20F,
            "跳变测试建立旧时间线")) {
        return false;
    }

    constexpr std::int64_t new_timestamp_us =
        kBaseTimestampUs + 700'000;

    /*
     * 新时间线同样提供120ms数据，使原有连续欠载恢复
     * 进入重新缓冲后能够立即满足默认启动深度。
     */
    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                120, 0.40F, new_timestamp_us))) {
        return false;
    }

    bool playback_clock_was_reset = false;
    bool new_audio_recovered = false;

    const auto deadline = std::chrono::steady_clock::now() + 500ms;

    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<float> output;

        if (!PopFrames(mixer, kChunkFrames, output)) {
            return false;
        }

        if (!mixer.GetPlaybackTimestampUs()) {
            playback_clock_was_reset = true;
        }

        if (IsConstantChunk(output, 0.40F)) {
            new_audio_recovered = true;
            break;
        }

        if (!IsConstantChunk(output, 0.0F)) {
            std::cerr << "  时间戳跳变恢复期间出现异常音频\n";
            return false;
        }
    }

    if (!playback_clock_was_reset) {
        std::cerr << "  持续欠载后播放时钟没有进入重新缓冲状态\n";
        return false;
    }

    if (!new_audio_recovered) {
        std::cerr << "  时间戳跳变后未在预期时间内恢复新音频\n";
        return false;
    }

    return ExpectClock(
        mixer,
        new_timestamp_us + 10'000,
        "时间戳跳变恢复后的播放时钟");
}

/**
 * @brief 测试播放时钟按照实际消费帧数推进
 */
bool TestPlaybackClockProgress()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                120, 0.15F, kBaseTimestampUs))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.15F,
            "播放时钟测试第一块音频")) {
        return false;
    }

    if (!ExpectClock(
            mixer,
            kBaseTimestampUs + 10'000,
            "消费第一块10ms音频")) {
        return false;
    }

    if (!ExpectNextFrames(
            mixer,
            4,
            0.15F,
            "分段消费4个采样帧")) {
        return false;
    }

    if (!ExpectClock(
            mixer,
            kBaseTimestampUs + 14'000,
            "消费4个采样帧后推进4ms")) {
        return false;
    }

    if (!ExpectNextFrames(
            mixer,
            6,
            0.15F,
            "消费当前Chunk剩余6个采样帧")) {
        return false;
    }

    return ExpectClock(
        mixer,
        kBaseTimestampUs + 20'000,
        "完整消费第二块后推进到20ms");
}

/**
 * @brief 测试删除用户后不再播放该用户声音
 */
bool TestRemoveUserBuffer()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                300, 0.20F, kBaseTimestampUs))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.20F,
            "删除用户测试建立播放时间线")) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_b",
            CreateConstantFrame(
                200,
                0.30F,
                kBaseTimestampUs + 100'000))) {
        return false;
    }

    for (int index = 0; index < 9; ++index) {
        if (!ExpectNextChunk(
                mixer,
                0.20F,
                "user_b开始前只播放user_a")) {
            return false;
        }
    }

    if (!ExpectNextChunk(
            mixer,
            0.50F,
            "删除前两名用户正常混音")) {
        return false;
    }

    mixer.RemoveUserBuffer("user_b");

    bool user_a_resumed = false;

    for (int index = 0; index < 8; ++index) {
        std::vector<float> output;

        if (!PopFrames(mixer, kChunkFrames, output)) {
            return false;
        }

        if (IsConstantChunk(output, 0.20F)) {
            user_a_resumed = true;
            break;
        }

        if (!IsConstantChunk(output, 0.0F)) {
            std::size_t mismatch_index = 0;
            IsConstantChunk(output, 0.0F, &mismatch_index);

            std::cerr << "  删除user_b后仍出现异常声音"
                      << "\n  样本位置: " << mismatch_index
                      << "\n  实际值: " << output[mismatch_index] << '\n';
            return false;
        }
    }

    if (!user_a_resumed) {
        std::cerr << "  删除user_b后user_a未在预期时间内恢复\n";
        return false;
    }

    return true;
}

/**
 * @brief 测试整体播放时间线重置
 */
bool TestResetPlaybackTimeline()
{
    RENDER::AudioMixer mixer;

    if (!InitializeMixer(mixer)) {
        return false;
    }

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                120, 0.20F, kBaseTimestampUs))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.20F,
            "重置测试建立旧播放时间线")) {
        return false;
    }

    mixer.ResetPlaybackTimeline();

    if (!ExpectClock(
            mixer,
            std::nullopt,
            "整体重置后清除播放时钟")) {
        return false;
    }

    std::vector<float> output;

    if (!PopFrames(mixer, kChunkFrames, output, false)) {
        return false;
    }

    if (!IsConstantChunk(output, 0.0F)) {
        std::cerr << "  整体重置后仍输出旧音频\n";
        return false;
    }

    if (!ExpectClock(
            mixer,
            std::nullopt,
            "没有新音频时不能重建播放时钟")) {
        return false;
    }

    constexpr std::int64_t new_timestamp_us = 5'000'000;

    if (!PushFrame(
            mixer,
            "user_a",
            CreateConstantFrame(
                120, 0.40F, new_timestamp_us))) {
        return false;
    }

    if (!WaitForInitialChunk(
            mixer,
            0.40F,
            "新音频重新建立播放时间线")) {
        return false;
    }

    return ExpectClock(
        mixer,
        new_timestamp_us + 10'000,
        "整体重置后的新播放时钟");
}

using TestFunction = bool (*)();

void RunTest(
    const std::string& test_name,
    TestFunction test_function,
    TestStatistics& statistics)
{
    std::cout << "\n========== " << test_name << " ==========\n";

    bool passed = false;

    try {
        passed = test_function();
    } catch (const std::exception& exception) {
        std::cerr << "  测试抛出异常: " << exception.what() << '\n';
    } catch (...) {
        std::cerr << "  测试抛出未知异常\n";
    }

    if (passed) {
        ++statistics.passed;
        std::cout << "测试结果: 通过\n";
    } else {
        ++statistics.failed;
        std::cout << "测试结果: 失败\n";
    }
}

} // namespace

int main()
{
    TestStatistics statistics;

    std::cout << "========== AudioMixer独立测试 ==========\n"
              << "音频格式: " << kSampleRate << "Hz/"
              << kChannels << "ch\n";

    RunTest(
        "自适应控制器初始状态",
        &TestAdaptiveControllerInitialState,
        statistics);

    RunTest(
        "稳定到达保持默认目标",
        &TestStableArrivalKeepsTarget,
        statistics);

    RunTest(
        "到达抖动提高目标缓存",
        &TestArrivalJitterIncreasesTarget,
        statistics);

    RunTest(
        "控制器时间线重置",
        &TestAdaptiveControllerReset,
        statistics);

    RunTest(
        "120ms启动缓存与短时欠载",
        &TestStartupBufferAndUnderrun,
        statistics);

    RunTest(
        "多用户按时间戳对齐混音",
        &TestMultiUserTimestampAlignment,
        statistics);

    RunTest(
        "音频小间隙补静音",
        &TestSmallGapFilledWithSilence,
        statistics);

    RunTest(
        "完全过期音频丢弃",
        &TestLateFrameDropped,
        statistics);

    RunTest(
        "时间戳跳变后重新缓冲",
        &TestTimestampDiscontinuityRecovery,
        statistics);

    RunTest(
        "播放时钟按消费帧推进",
        &TestPlaybackClockProgress,
        statistics);

    RunTest(
        "删除用户音频状态",
        &TestRemoveUserBuffer,
        statistics);

    RunTest(
        "整体播放时间线重置",
        &TestResetPlaybackTimeline,
        statistics);

    std::cout << "\n========== 测试汇总 ==========\n"
              << "通过: " << statistics.passed << '\n'
              << "失败: " << statistics.failed << '\n';

    return statistics.failed == 0 ? 0 : 1;
}