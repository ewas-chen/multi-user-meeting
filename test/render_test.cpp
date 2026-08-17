#include "IRenderEngine.h"
#include "utils/logManager.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

using TestClock = std::chrono::steady_clock;

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kVideoWidth = 640;
constexpr int kVideoHeight = 480;
constexpr int kVideoFps = 30;

/*
 * 文件驱动测试需要预先准备一段音频。
 * 否则测试线程和播放设备以相同速率生产、消费，
 * 任意调度抖动都会导致AudioMixer欠载。
 */
constexpr int kAudioPrebufferDurationMs = 120;

constexpr int kAudioChunkDurationMs = 10;
constexpr int kAudioChunkFrames =
    kSampleRate * kAudioChunkDurationMs / 1000;

constexpr int kTestDurationSeconds = 59;
constexpr const char* kTestUserName = "local_render_test_user";

struct RenderTestConfig
{
    std::filesystem::path video_file{
        "/home/ewas/VC_code/meeting-client/source/"
        "camera_640x480_i420.yuv"
    };

    std::filesystem::path audio_file{
        "/home/ewas/VC_code/meeting-client/source/"
        "microphone_48000hz_2ch_f32le.pcm"
    };

    int sample_rate{kSampleRate};
    int channels{kChannels};
    int video_width{kVideoWidth};
    int video_height{kVideoHeight};
    int video_fps{kVideoFps};
    int test_duration_seconds{kTestDurationSeconds};
};

struct RenderTestStatistics
{
    std::size_t video_frames_read{0};
    std::size_t video_frames_pushed{0};
    std::size_t video_push_failures{0};

    std::size_t audio_chunks_read{0};
    std::size_t audio_chunks_pushed{0};
    std::size_t audio_frames_pushed{0};
    std::size_t audio_push_failures{0};

    std::size_t render_calls{0};
    std::size_t render_successes{0};
    std::size_t render_failures{0};

    std::size_t viewport_updates{0};
    std::size_t viewport_update_failures{0};
};

class GlfwTerminateGuard final
{
public:
    GlfwTerminateGuard() = default;

    ~GlfwTerminateGuard()
    {
        glfwTerminate();
    }

    GlfwTerminateGuard(const GlfwTerminateGuard&) = delete;
    GlfwTerminateGuard& operator=(const GlfwTerminateGuard&) = delete;
};

struct GlfwWindowDeleter
{
    void operator()(GLFWwindow* window) const noexcept
    {
        if (window)
        {
            glfwDestroyWindow(window);
        }
    }
};

using GlfwWindowPtr =
    std::unique_ptr<GLFWwindow, GlfwWindowDeleter>;

bool InitializeTestLogger()
{
    logConfig config;
    config.logger_name = "render_test";
    config.console_output = true;
    config.file_output = false;
    config.level = spdlog::level::info;

    if (!Logger::Instance().Init(config))
    {
        std::cerr << "渲染测试日志初始化失败\n";
        return false;
    }

    return true;
}

RenderTestConfig ParseTestConfig(int argc, char* argv[])
{
    RenderTestConfig config;

    if (argc >= 2)
    {
        config.video_file = argv[1];
    }

    if (argc >= 3)
    {
        config.audio_file = argv[2];
    }

    return config;
}

void PrintTestConfig(const RenderTestConfig& config)
{
    std::cout
        << "\n========== 渲染测试配置 ==========\n"
        << "视频文件: " << config.video_file.string() << '\n'
        << "音频文件: " << config.audio_file.string() << '\n'
        << "视频格式: I420 "
        << config.video_width << 'x' << config.video_height
        << " @ " << config.video_fps << " FPS\n"
        << "音频格式: Float32交错PCM "
        << config.sample_rate << " Hz, "
        << config.channels << " channels\n"
        << "测试时长: "
        << config.test_duration_seconds << " 秒\n";
}

bool OpenInputFiles(
    const RenderTestConfig& config,
    std::ifstream& video_stream,
    std::ifstream& audio_stream)
{
    if (!std::filesystem::exists(config.video_file))
    {
        std::cerr
            << "视频测试文件不存在: "
            << config.video_file
            << '\n';
        return false;
    }

    if (!std::filesystem::exists(config.audio_file))
    {
        std::cerr
            << "音频测试文件不存在: "
            << config.audio_file
            << '\n';
        return false;
    }

    video_stream.open(config.video_file, std::ios::binary);

    if (!video_stream.is_open())
    {
        std::cerr
            << "无法打开视频测试文件: "
            << config.video_file
            << '\n';
        return false;
    }

    audio_stream.open(config.audio_file, std::ios::binary);

    if (!audio_stream.is_open())
    {
        std::cerr
            << "无法打开音频测试文件: "
            << config.audio_file
            << '\n';
        return false;
    }

    const std::uintmax_t video_file_size =
        std::filesystem::file_size(config.video_file);

    const std::uintmax_t audio_file_size =
        std::filesystem::file_size(config.audio_file);

    const std::size_t video_frame_size =
        static_cast<std::size_t>(config.video_width) *
        static_cast<std::size_t>(config.video_height) * 3U / 2U;

    const std::size_t audio_frame_size =
        static_cast<std::size_t>(config.channels) *
        sizeof(float);

    if (video_frame_size == 0 ||
        video_file_size < video_frame_size)
    {
        std::cerr << "视频文件不足一帧I420数据\n";
        return false;
    }

    if (audio_frame_size == 0 ||
        audio_file_size < audio_frame_size)
    {
        std::cerr << "音频文件不包含有效Float32 PCM数据\n";
        return false;
    }

    const std::uintmax_t complete_video_frames =
        video_file_size / video_frame_size;

    const std::uintmax_t audio_frames_per_channel =
        audio_file_size / audio_frame_size;

    const double video_duration =
        static_cast<double>(complete_video_frames) /
        static_cast<double>(config.video_fps);

    const double audio_duration =
        static_cast<double>(audio_frames_per_channel) /
        static_cast<double>(config.sample_rate);

    std::cout
        << "\n========== 输入文件检查 ==========\n"
        << "视频文件字节数: " << video_file_size << '\n'
        << "完整视频帧数: " << complete_video_frames << '\n'
        << "视频文件时长: " << video_duration << " 秒\n"
        << "音频文件字节数: " << audio_file_size << '\n'
        << "每声道音频帧数: " << audio_frames_per_channel << '\n'
        << "音频文件时长: " << audio_duration << " 秒\n";

    if ((video_file_size % video_frame_size) != 0)
    {
        std::cout
            << "警告：视频文件末尾存在不足一帧的数据，"
            << "测试时会忽略\n";
    }

    if ((audio_file_size % audio_frame_size) != 0)
    {
        std::cout
            << "警告：音频文件末尾存在不完整采样，"
            << "测试时会忽略\n";
    }

    if (video_duration < config.test_duration_seconds)
    {
        std::cerr << "视频文件时长小于测试时长\n";
        return false;
    }

    if (audio_duration < config.test_duration_seconds)
    {
        std::cerr << "音频文件时长小于测试时长\n";
        return false;
    }

    return true;
}

void GlfwErrorCallback(
    int error_code,
    const char* description)
{
    std::cerr
        << "GLFW错误: code=" << error_code
        << ", description="
        << (description ? description : "")
        << '\n';
}

bool InitializeGlfw()
{
    glfwSetErrorCallback(GlfwErrorCallback);

    if (glfwInit() != GLFW_TRUE)
    {
        std::cerr << "GLFW初始化失败\n";
        return false;
    }

    return true;
}

GLFWwindow* CreateOpenGLWindow(int width, int height)
{
    /*
     * 当前VideoShader使用GLSL 330，
     * 因此创建OpenGL 3.3 Core Context。
     */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(
        GLFW_OPENGL_PROFILE,
        GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(
        width,
        height,
        "RenderEngine I420 Test",
        nullptr,
        nullptr);

    if (!window)
    {
        std::cerr << "OpenGL测试窗口创建失败\n";
        return nullptr;
    }

    /*
     * AddUser会创建OpenGL资源，因此必须先绑定Context。
     */
    glfwMakeContextCurrent(window);

    /*
     * 开启垂直同步，避免测试循环无意义地占满CPU。
     */
    glfwSwapInterval(1);

    return window;
}

bool InitializeRenderEngine(
    RENDER::IRenderEngine& render_engine,
    const RenderTestConfig& config)
{
    if (!render_engine.Initialize(
            config.sample_rate,
            config.channels,
            config.video_width,
            config.video_height))
    {
        std::cerr << "RenderEngine初始化失败\n";
        return false;
    }

    std::cout << "\nRenderEngine初始化成功\n";
    return true;
}

bool TestSpeakerFunctions(
    RENDER::IRenderEngine& render_engine)
{
    const std::vector<RENDER::AudioSpeaker> speakers =
        render_engine.GetSpeakerDevices();

    std::cout
        << "\n========== 扬声器设备测试 ==========\n"
        << "发现扬声器数量: "
        << speakers.size()
        << '\n';

    for (const auto& speaker : speakers)
    {
        std::cout
            << "设备名称: " << speaker.name << '\n'
            << "设备ID: " << speaker.device_id << '\n'
            << "是否默认设备: "
            << (speaker.is_default ? "是" : "否")
            << '\n';
    }

    if (speakers.empty())
    {
        std::cerr << "没有发现可用扬声器\n";
        return false;
    }

    std::string current_device_id;

    if (!render_engine.GetCurrentAudioSpeaker(
            current_device_id))
    {
        std::cerr << "获取当前扬声器失败\n";
        return false;
    }

    std::cout
        << "当前扬声器ID: "
        << current_device_id
        << '\n';

    auto selected_speaker = std::find_if(
        speakers.begin(),
        speakers.end(),
        [](const RENDER::AudioSpeaker& speaker)
        {
            return speaker.is_default;
        });

    if (selected_speaker == speakers.end())
    {
        selected_speaker = speakers.begin();
    }

    if (!render_engine.UpdateAudioSpeaker(
            selected_speaker->device_id))
    {
        std::cerr
            << "切换扬声器失败: "
            << selected_speaker->name
            << '\n';
        return false;
    }

    std::cout
        << "已选择扬声器: "
        << selected_speaker->name
        << '\n';

    return true;
}

bool AddLocalTestUser(
    RENDER::IRenderEngine& render_engine)
{
    /*
     * render_test只验证本地渲染链路，因此使用is_local=true。
     * 本地用户始终选择最新视频帧，不进入远端AVSyncController。
     */
    if (!render_engine.AddUser(
            kTestUserName,
            true))
    {
        std::cerr << "添加本地渲染用户失败\n";
        return false;
    }

    std::cout
        << "\n本地渲染用户添加成功: "
        << kTestUserName
        << '\n';

    return true;
}

bool ReadI420Frame(
    std::ifstream& video_stream,
    int width,
    int height,
    std::int64_t timestamp_us,
    std::shared_ptr<RENDER::I420Frame>& output)
{
    if (width <= 0 ||
        height <= 0 ||
        (width % 2) != 0 ||
        (height % 2) != 0 ||
        timestamp_us <= 0)
    {
        return false;
    }

    const std::size_t y_plane_size =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    const std::size_t uv_plane_size =
        static_cast<std::size_t>(width / 2) *
        static_cast<std::size_t>(height / 2);

    auto frame =
        std::make_shared<RENDER::I420Frame>();

    frame->width = width;
    frame->height = height;
    frame->timestamp_us = timestamp_us;

    frame->data[0] =
        std::shared_ptr<std::uint8_t[]>(
            new std::uint8_t[y_plane_size]);

    frame->data[1] =
        std::shared_ptr<std::uint8_t[]>(
            new std::uint8_t[uv_plane_size]);

    frame->data[2] =
        std::shared_ptr<std::uint8_t[]>(
            new std::uint8_t[uv_plane_size]);

    video_stream.read(
        reinterpret_cast<char*>(
            frame->data[0].get()),
        static_cast<std::streamsize>(
            y_plane_size));

    if (video_stream.gcount() !=
        static_cast<std::streamsize>(
            y_plane_size))
    {
        return false;
    }

    video_stream.read(
        reinterpret_cast<char*>(
            frame->data[1].get()),
        static_cast<std::streamsize>(
            uv_plane_size));

    if (video_stream.gcount() !=
        static_cast<std::streamsize>(
            uv_plane_size))
    {
        return false;
    }

    video_stream.read(
        reinterpret_cast<char*>(
            frame->data[2].get()),
        static_cast<std::streamsize>(
            uv_plane_size));

    if (video_stream.gcount() !=
        static_cast<std::streamsize>(
            uv_plane_size))
    {
        return false;
    }

    if (!frame->IsValid())
    {
        return false;
    }

    output = std::move(frame);
    return true;
}

bool ReadAudioFrame(
    std::ifstream& audio_stream,
    int sample_rate,
    int channels,
    int requested_frames,
    std::int64_t timestamp_us,
    std::shared_ptr<RENDER::AudioFrame>& output)
{
    if (sample_rate <= 0 ||
        channels <= 0 ||
        requested_frames <= 0 ||
        timestamp_us <= 0)
    {
        return false;
    }

    const std::size_t bytes_per_frame =
        static_cast<std::size_t>(channels) *
        sizeof(float);

    const std::size_t requested_bytes =
        static_cast<std::size_t>(requested_frames) *
        bytes_per_frame;

    auto frame =
        std::make_shared<RENDER::AudioFrame>();

    frame->data =
        std::shared_ptr<std::uint8_t[]>(
            new std::uint8_t[requested_bytes]);

    audio_stream.read(
        reinterpret_cast<char*>(
            frame->data.get()),
        static_cast<std::streamsize>(
            requested_bytes));

    const std::streamsize bytes_read =
        audio_stream.gcount();

    if (bytes_read <= 0)
    {
        return false;
    }

    const std::size_t complete_frames =
        static_cast<std::size_t>(bytes_read) /
        bytes_per_frame;

    if (complete_frames == 0)
    {
        return false;
    }

    frame->samples =
        static_cast<int>(complete_frames);
    frame->channels = channels;
    frame->sample_rate = sample_rate;
    frame->timestamp_us = timestamp_us;

    if (!frame->IsValid())
    {
        return false;
    }

    output = std::move(frame);
    return true;
}

bool UpdateViewportIfNeeded(
    GLFWwindow* window,
    RENDER::IRenderEngine& render_engine,
    int& current_width,
    int& current_height,
    RenderTestStatistics& statistics)
{
    int framebuffer_width = 0;
    int framebuffer_height = 0;

    glfwGetFramebufferSize(
        window,
        &framebuffer_width,
        &framebuffer_height);

    /*
     * 窗口最小化时Framebuffer可能为0x0。
     * 等待窗口恢复即可，不视为错误。
     */
    if (framebuffer_width <= 0 ||
        framebuffer_height <= 0)
    {
        return true;
    }

    if (framebuffer_width == current_width &&
        framebuffer_height == current_height)
    {
        return true;
    }

    if (!render_engine.UpdateUserVideoSize(
            kTestUserName,
            framebuffer_width,
            framebuffer_height))
    {
        ++statistics.viewport_update_failures;
        return false;
    }

    current_width = framebuffer_width;
    current_height = framebuffer_height;
    ++statistics.viewport_updates;

    std::cout
        << "Viewport更新为: "
        << framebuffer_width
        << 'x'
        << framebuffer_height
        << '\n';

    return true;
}

void HandleWindowInput(GLFWwindow* window)
{
    if (glfwGetKey(
            window,
            GLFW_KEY_ESCAPE) == GLFW_PRESS)
    {
        glfwSetWindowShouldClose(
            window,
            GLFW_TRUE);
    }
}

bool RunAudioVideoRenderLoop(
    GLFWwindow* window,
    RENDER::IRenderEngine& render_engine,
    std::ifstream& video_stream,
    std::ifstream& audio_stream,
    const RenderTestConfig& config,
    RenderTestStatistics& statistics)
{
    const auto test_start = TestClock::now();

    const auto test_end =
        test_start +
        std::chrono::seconds(
            config.test_duration_seconds);

    auto next_video_time = test_start;
    auto next_audio_time = test_start;

    /*
     * 音频和视频必须从同一个正数单调时钟时间戳开始。
     *
     * 原测试从0开始，而TimestampedAudioBuffer明确拒绝
     * timestamp_us <= 0，因此首个音频块必然推送失败。
     */
    const std::int64_t media_start_timestamp_us =
        std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                test_start.time_since_epoch())
                .count());

    std::int64_t next_video_timestamp_us =
        media_start_timestamp_us;

    std::int64_t next_audio_timestamp_us =
        media_start_timestamp_us;

    bool video_finished = false;
    bool audio_finished = false;

    int viewport_width = 0;
    int viewport_height = 0;

    std::cout
        << "\n========== 开始音视频渲染 ==========\n"
        << "测试持续 "
        << config.test_duration_seconds
        << " 秒，按ESC可以提前退出\n"
        << "统一媒体时间线起点: "
        << media_start_timestamp_us
        << " us\n";

    while (!glfwWindowShouldClose(window) &&
           TestClock::now() < test_end)
    {
        glfwPollEvents();
        HandleWindowInput(window);

        if (!UpdateViewportIfNeeded(
                window,
                render_engine,
                viewport_width,
                viewport_height,
                statistics))
        {
            std::cerr << "更新视频Viewport失败\n";
            return false;
        }

        const auto now = TestClock::now();

        /*
         * 按视频帧率推送I420帧。
         * 如果测试线程短暂延迟，循环会补充已到期的帧。
         */
        while (!video_finished &&
               now >= next_video_time)
        {
            std::shared_ptr<RENDER::I420Frame>
                video_frame;

            if (!ReadI420Frame(
                    video_stream,
                    config.video_width,
                    config.video_height,
                    next_video_timestamp_us,
                    video_frame))
            {
                video_finished = true;
                std::cout << "视频测试文件读取完成\n";
                break;
            }

            ++statistics.video_frames_read;

            if (render_engine.PushVideoFrame(
                    kTestUserName,
                    video_frame))
            {
                ++statistics.video_frames_pushed;
            }
            else
            {
                ++statistics.video_push_failures;
            }

            const std::int64_t video_step_us =
                1'000'000LL /
                config.video_fps;

            next_video_timestamp_us +=
                video_step_us;

            next_video_time +=
                std::chrono::microseconds(
                    video_step_us);
        }

        /*
        * 始终使送入AudioMixer的音频领先当前墙钟120ms。
        *
        * 这只改变测试文件的供给时机，不改变媒体时间戳：
        * 音频和视频仍然从media_start_timestamp_us开始，
        * AudioPlaybackClock仍根据扬声器实际消费量推进。
        */
        const auto audio_feed_deadline =
            std::min(
                now + std::chrono::milliseconds(
                        kAudioPrebufferDurationMs),
                test_end);

        while (!audio_finished &&
            next_audio_time < audio_feed_deadline)
        {
            std::shared_ptr<RENDER::AudioFrame> audio_frame;

            if (!ReadAudioFrame(
                    audio_stream,
                    config.sample_rate,
                    config.channels,
                    kAudioChunkFrames,
                    next_audio_timestamp_us,
                    audio_frame))
            {
                audio_finished = true;
                std::cout << "音频测试文件读取完成\n";
                break;
            }

            ++statistics.audio_chunks_read;

            const int audio_frame_count =
                audio_frame->samples;

            if (render_engine.PushAudioFrame(
                    kTestUserName,
                    audio_frame))
            {
                ++statistics.audio_chunks_pushed;

                statistics.audio_frames_pushed +=
                    static_cast<std::size_t>(
                        audio_frame_count);
            }
            else
            {
                ++statistics.audio_push_failures;
            }

            const std::int64_t audio_step_us =
                static_cast<std::int64_t>(
                    audio_frame_count) *
                1'000'000LL /
                config.sample_rate;

            next_audio_timestamp_us += audio_step_us;

            next_audio_time +=
                std::chrono::microseconds(
                    std::max<std::int64_t>(
                        audio_step_us,
                        1));
        }

        if (viewport_width <= 0 ||
            viewport_height <= 0)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
            continue;
        }

        ++statistics.render_calls;

        if (render_engine.RenderUser(
                kTestUserName))
        {
            ++statistics.render_successes;
        }
        else
        {
            ++statistics.render_failures;
        }

        /*
         * RenderUser只提交OpenGL绘制命令，
         * GLFW负责交换前后缓冲区。
         */
        glfwSwapBuffers(window);
    }

    /*
    * 播放测试循环中已经提前送入的尾部音频。
    */
    std::this_thread::sleep_for(
        std::chrono::milliseconds(
            kAudioPrebufferDurationMs + 50));

    return true;
}

bool PrintTestResult(
    const RenderTestStatistics& statistics)
{
    std::cout
        << "\n========== 渲染测试结果 ==========\n"
        << "读取视频帧数: "
        << statistics.video_frames_read << '\n'
        << "成功推送视频帧数: "
        << statistics.video_frames_pushed << '\n'
        << "视频推送失败数: "
        << statistics.video_push_failures << '\n'
        << "读取音频块数: "
        << statistics.audio_chunks_read << '\n'
        << "成功推送音频块数: "
        << statistics.audio_chunks_pushed << '\n'
        << "成功推送音频帧数（每声道）: "
        << statistics.audio_frames_pushed << '\n'
        << "音频推送失败数: "
        << statistics.audio_push_failures << '\n'
        << "渲染调用次数: "
        << statistics.render_calls << '\n'
        << "渲染成功次数: "
        << statistics.render_successes << '\n'
        << "渲染失败次数: "
        << statistics.render_failures << '\n'
        << "Viewport更新次数: "
        << statistics.viewport_updates << '\n'
        << "Viewport更新失败次数: "
        << statistics.viewport_update_failures << '\n';

    const bool passed =
        statistics.video_frames_pushed > 0 &&
        statistics.audio_chunks_pushed > 0 &&
        statistics.render_successes > 0 &&
        statistics.video_push_failures == 0 &&
        statistics.audio_push_failures == 0 &&
        statistics.render_failures == 0 &&
        statistics.viewport_update_failures == 0;

    std::cout
        << "最终结果: "
        << (passed ? "通过" : "失败")
        << '\n';

    return passed;
}

bool RemoveLocalTestUser(
    RENDER::IRenderEngine& render_engine)
{
    if (!render_engine.RemoveUser(
            kTestUserName))
    {
        std::cerr << "删除本地渲染用户失败\n";
        return false;
    }

    std::cout << "本地渲染用户删除成功\n";
    return true;
}

bool RunRenderModuleTest(
    const RenderTestConfig& config)
{
    std::ifstream video_stream;
    std::ifstream audio_stream;

    if (!OpenInputFiles(
            config,
            video_stream,
            audio_stream))
    {
        return false;
    }

    if (!InitializeGlfw())
    {
        return false;
    }

    GlfwTerminateGuard glfw_guard;

    GlfwWindowPtr window(
        CreateOpenGLWindow(
            config.video_width,
            config.video_height));

    if (!window)
    {
        return false;
    }

    /*
     * RenderEngine必须在GLFW窗口之后创建，
     * 确保销毁时OpenGL Context仍然有效。
     */
    auto render_engine =
        RENDER::IRenderEngine::CreateRenderEngine();

    if (!render_engine)
    {
        std::cerr << "创建RenderEngine失败\n";
        return false;
    }

    if (!InitializeRenderEngine(
            *render_engine,
            config))
    {
        return false;
    }

    if (!TestSpeakerFunctions(
            *render_engine))
    {
        return false;
    }

    if (!AddLocalTestUser(
            *render_engine))
    {
        return false;
    }

    RenderTestStatistics statistics;

    const bool loop_succeeded =
        RunAudioVideoRenderLoop(
            window.get(),
            *render_engine,
            video_stream,
            audio_stream,
            config,
            statistics);

    const bool result_passed =
        PrintTestResult(statistics);

    /*
     * 当前线程仍持有OpenGL Context，
     * 此时可以安全释放用户的视频资源。
     */
    const bool user_removed =
        RemoveLocalTestUser(
            *render_engine);

    render_engine->Uninitialize();
    render_engine.reset();

    std::cout << "RenderEngine反初始化完成\n";

    return loop_succeeded &&
           result_passed &&
           user_removed;
}

} // namespace

int main(int argc, char* argv[])
{
    if (!InitializeTestLogger())
    {
        return 1;
    }

    const RenderTestConfig config =
        ParseTestConfig(argc, argv);

    PrintTestConfig(config);

    const bool test_succeeded =
        RunRenderModuleTest(config);

    Logger::Instance().Shutdown();

    return test_succeeded ? 0 : 1;
}