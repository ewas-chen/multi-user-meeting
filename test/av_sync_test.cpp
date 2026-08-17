#include "AVSyncController.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct TestStatistics
{
    int passed{0};
    int failed{0};
};

const char* ActionName(RENDER::AVSyncAction action)
{
    switch (action) {
    case RENDER::AVSyncAction::kWait:
        return "Wait";
    case RENDER::AVSyncAction::kRender:
        return "Render";
    case RENDER::AVSyncAction::kDrop:
        return "Drop";
    }

    return "Unknown";
}

bool CheckDecision(
    const std::string& test_name,
    const RENDER::AVSyncDecision& decision,
    RENDER::AVSyncAction expected_action,
    bool expected_synchronized,
    bool expected_timeline_reset,
    TestStatistics& statistics)
{
    const bool passed =
        decision.action == expected_action &&
        decision.synchronized == expected_synchronized &&
        decision.timeline_reset == expected_timeline_reset;

    std::cout
        << "\n测试: " << test_name << '\n'
        << "  实际动作: " << ActionName(decision.action) << '\n'
        << "  期望动作: " << ActionName(expected_action) << '\n'
        << "  音视频差值: " << decision.av_delta_us << " us\n"
        << "  使用音频时钟: "
        << (decision.synchronized ? "是" : "否") << '\n'
        << "  时间线重置: "
        << (decision.timeline_reset ? "是" : "否") << '\n'
        << "  测试结果: "
        << (passed ? "通过" : "失败") << '\n';

    if (passed) {
        ++statistics.passed;
    } else {
        ++statistics.failed;
    }

    return passed;
}

/**
 * @brief 测试音频播放时钟不存在时的降级行为
 *
 * 纯视频会议或者音频尚未开始播放时，视频不能一直等待，
 * 控制器应直接允许视频渲染。
 */
void TestWithoutAudioClock(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    const auto decision =
        controller.EvaluateVideoFrame(
            1'000'000,
            std::nullopt);

    CheckDecision(
        "没有音频时钟时直接渲染",
        decision,
        RENDER::AVSyncAction::kRender,
        false,
        false,
        statistics);
}

/**
 * @brief 测试首帧时间基准建立
 *
 * 视频和音频原始时间戳相差100ms。默认启用首帧对齐，
 * 因此第一次判断会把100ms作为固定时间偏移，
 * 修正后的音视频差值应当为0。
 */
void TestFirstFrameAlignment(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    const auto decision =
        controller.EvaluateVideoFrame(
            1'000'000,
            900'000);

    const bool decision_passed =
        CheckDecision(
            "首帧建立同步基准",
            decision,
            RENDER::AVSyncAction::kRender,
            true,
            false,
            statistics);

    if (decision_passed &&
        decision.av_delta_us != 0) {
        std::cout
            << "  额外检查失败: 首帧修正差值应为0\n";

        --statistics.passed;
        ++statistics.failed;
    }
}

/**
 * @brief 测试视频早到时等待
 *
 * 首帧建立100ms固定偏移后，第二帧修正后的时间差为：
 *
 *     (1,120,000 - 920,000) - 100,000
 *     = 100,000us
 *
 * 视频比音频早100ms，大于默认50ms阈值，因此应等待。
 */
void TestEarlyVideoWait(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    controller.EvaluateVideoFrame(
        1'000'000,
        900'000);

    const auto decision =
        controller.EvaluateVideoFrame(
            1'120'000,
            920'000);

    CheckDecision(
        "视频早到100ms时等待",
        decision,
        RENDER::AVSyncAction::kWait,
        true,
        false,
        statistics);
}

/**
 * @brief 测试正常同步范围内的视频渲染
 *
 * 修正后的时间差为30ms，处于：
 *
 *     -80ms <= delta <= 50ms
 *
 * 因此应正常渲染。
 */
void TestVideoInSyncRange(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    controller.EvaluateVideoFrame(
        1'000'000,
        900'000);

    const auto decision =
        controller.EvaluateVideoFrame(
            1'050'000,
            920'000);

    CheckDecision(
        "音视频差值30ms时正常渲染",
        decision,
        RENDER::AVSyncAction::kRender,
        true,
        false,
        statistics);
}

/**
 * @brief 测试视频晚到时丢弃
 *
 * 首帧固定偏移为100ms，第二次判断的原始时间差为0，
 * 修正后的时间差为-100ms。视频落后音频100ms，
 * 超过默认80ms阈值，因此应丢弃。
 */
void TestLateVideoDrop(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    controller.EvaluateVideoFrame(
        1'000'000,
        900'000);

    const auto decision =
        controller.EvaluateVideoFrame(
            1'100'000,
            1'100'000);

    CheckDecision(
        "视频晚到100ms时丢弃",
        decision,
        RENDER::AVSyncAction::kDrop,
        true,
        false,
        statistics);
}

/**
 * @brief 测试时间戳发生大幅跳变后的时间线重建
 *
 * 第二组时间戳的原始音视频差值突然变化超过500ms，
 * 控制器应重新建立同步基准，而不是持续等待或丢帧。
 */
void TestTimelineDiscontinuity(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    controller.EvaluateVideoFrame(
        1'000'000,
        900'000);

    const auto decision =
        controller.EvaluateVideoFrame(
            2'000'000,
            900'000);

    CheckDecision(
        "时间戳大幅跳变时重建时间线",
        decision,
        RENDER::AVSyncAction::kRender,
        true,
        true,
        statistics);
}

/**
 * @brief 测试视频时间戳明显向后跳变
 *
 * 视频时间戳向后跳变200ms，超过默认100ms阈值，
 * 控制器应重新建立时间基准。
 */
void TestBackwardTimestampJump(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    controller.EvaluateVideoFrame(
        1'000'000,
        1'000'000);

    const auto decision =
        controller.EvaluateVideoFrame(
            800'000,
            1'010'000);

    CheckDecision(
        "视频时间戳向后跳变时重建时间线",
        decision,
        RENDER::AVSyncAction::kRender,
        true,
        true,
        statistics);
}

/**
 * @brief 测试禁用首帧对齐后的公共时间线模式
 *
 * 当Transport已经通过RTCP SR把音视频时间戳映射到相同
 * 公共时间线后，可以关闭首帧对齐。此时时间戳直接比较。
 */
void TestCommonTimelineMode(TestStatistics& statistics)
{
    RENDER::AVSyncConfig config;
    config.align_first_frame = false;

    RENDER::AVSyncController controller(config);

    const auto decision =
        controller.EvaluateVideoFrame(
            1'100'000,
            1'000'000);

    CheckDecision(
        "公共时间线模式下视频早到100ms",
        decision,
        RENDER::AVSyncAction::kWait,
        true,
        false,
        statistics);
}

/**
 * @brief 测试配置参数校验
 */
void TestInvalidConfiguration(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    RENDER::AVSyncConfig invalid_config;
    invalid_config.video_early_threshold_us = -1;

    const bool rejected =
        !controller.SetConfig(invalid_config);

    std::cout
        << "\n测试: 拒绝无效同步参数\n"
        << "  测试结果: "
        << (rejected ? "通过" : "失败") << '\n';

    if (rejected) {
        ++statistics.passed;
    } else {
        ++statistics.failed;
    }
}

/**
 * @brief 测试同步统计数据
 */
void TestStatisticsResult(TestStatistics& statistics)
{
    RENDER::AVSyncController controller;

    // 首帧：Render
    controller.EvaluateVideoFrame(
        1'000'000,
        900'000);

    // 修正后+100ms：Wait
    controller.EvaluateVideoFrame(
        1'120'000,
        920'000);

    // 修正后-100ms：Drop
    controller.EvaluateVideoFrame(
        1'100'000,
        1'100'000);

    // 无音频时钟：非同步Render
    controller.EvaluateVideoFrame(
        1'200'000,
        std::nullopt);

    const auto result =
        controller.GetStatistics();

    const bool passed =
        result.decision_count == 4 &&
        result.render_decision_count == 2 &&
        result.wait_decision_count == 1 &&
        result.dropped_frame_count == 1 &&
        result.unsynchronized_render_count == 1;

    std::cout
        << "\n测试: 同步统计数据\n"
        << "  决策总数: "
        << result.decision_count << '\n'
        << "  渲染决策: "
        << result.render_decision_count << '\n'
        << "  等待决策: "
        << result.wait_decision_count << '\n'
        << "  丢帧决策: "
        << result.dropped_frame_count << '\n'
        << "  非同步渲染: "
        << result.unsynchronized_render_count << '\n'
        << "  平滑音视频差值: "
        << result.smoothed_av_delta_us << " us\n"
        << "  最大绝对差值: "
        << result.maximum_abs_av_delta_us << " us\n"
        << "  测试结果: "
        << (passed ? "通过" : "失败") << '\n';

    if (passed) {
        ++statistics.passed;
    } else {
        ++statistics.failed;
    }
}

} // namespace

int main()
{
    std::cout
        << "========== AVSyncController独立测试 ==========\n";

    TestStatistics statistics;

    TestWithoutAudioClock(statistics);
    TestFirstFrameAlignment(statistics);
    TestEarlyVideoWait(statistics);
    TestVideoInSyncRange(statistics);
    TestLateVideoDrop(statistics);
    TestTimelineDiscontinuity(statistics);
    TestBackwardTimestampJump(statistics);
    TestCommonTimelineMode(statistics);
    TestInvalidConfiguration(statistics);
    TestStatisticsResult(statistics);

    std::cout
        << "\n========== 测试总结 ==========\n"
        << "通过数量: " << statistics.passed << '\n'
        << "失败数量: " << statistics.failed << '\n'
        << "最终结果: "
        << (statistics.failed == 0 ? "通过" : "失败")
        << '\n';

    return statistics.failed == 0 ? 0 : 1;
}