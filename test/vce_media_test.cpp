#include "CaptureManager.h"
#include "ICaptureDataCallback.h"
#include "ITransportDataCallback.h"
#include "RenderManager.h"
#include "TransportManager.h"
#include "VceTypes.h"

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

/*
 * 这里比较的是最近收到的音频和视频媒体时间戳，
 * 不是AVSyncController最终校正后的播放差值。
 *
 * 500ms用于识别时间线没有统一或某一路媒体明显停滞。
 * 实际等待、渲染和丢帧仍由AVSyncController负责。
 */
constexpr std::int64_t kMaxRemoteTimelineDifferenceUs = 500'000;

std::atomic<bool> g_stop_requested{false};

void SignalHandler(int)
{
    g_stop_requested.store(true, std::memory_order_release);
}

bool IsSuccess(VCE::Result result)
{
    return result == VCE::kRet_SUCCESS;
}

void UpdateMaximum(
    std::atomic<std::int64_t>& target,
    std::int64_t candidate)
{
    std::int64_t current =
        target.load(std::memory_order_relaxed);

    while (candidate > current &&
           !target.compare_exchange_weak(
               current,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed))
    {
    }
}

void UpdateMinimum(
    std::atomic<std::int64_t>& target,
    std::int64_t candidate)
{
    std::int64_t current =
        target.load(std::memory_order_relaxed);

    while (candidate < current &&
           !target.compare_exchange_weak(
               current,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed))
    {
    }
}

std::string Trim(const std::string& value)
{
    const auto begin =
        value.find_first_not_of(" \t\r\n");

    if (begin == std::string::npos)
    {
        return {};
    }

    const auto end =
        value.find_last_not_of(" \t\r\n");

    return value.substr(begin, end - begin + 1);
}

bool ParseBool(std::string value, bool& result)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch)
        {
            return static_cast<char>(
                std::tolower(ch));
        });

    if (value == "true" ||
        value == "1" ||
        value == "yes" ||
        value == "on")
    {
        result = true;
        return true;
    }

    if (value == "false" ||
        value == "0" ||
        value == "no" ||
        value == "off")
    {
        result = false;
        return true;
    }

    return false;
}

struct MediaTestConfig
{
    std::string mode;
    std::string local_user_id;
    std::string remote_user_id;
    std::string room_id;

    std::string push_server_url;
    std::string pull_server_url;
    std::string app_name{"live"};
    std::string rtc_external_address;
    std::string whip_secret;

    std::string camera_device_id;
    std::string camera_video_format{"YUYV"};
    std::string microphone_device_id;
    std::string speaker_device_id;

    int sample_rate{48000};
    int channels{2};
    int video_width{640};
    int video_height{480};
    int video_fps{30};

    int duration_seconds{60};
    int subscribe_retry_seconds{5};

    bool enable_video{true};
    bool enable_audio{true};
    bool expect_remote_video{true};
    bool expect_remote_audio{true};

    /*
     * 保存原始接收数据仅用于诊断。
     * 开启后会在传输回调线程同步写文件。
     */
    bool save_received_media{false};
    std::string received_video_file;
    std::string received_audio_file;
};

bool LoadConfig(
    const std::string& file_path,
    MediaTestConfig& config)
{
    std::ifstream input(file_path);

    if (!input.is_open())
    {
        std::cerr
            << "无法打开配置文件: "
            << file_path
            << '\n';
        return false;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;

    while (std::getline(input, line))
    {
        line = Trim(line);

        if (line.empty() ||
            line.front() == '#')
        {
            continue;
        }

        const auto separator = line.find('=');

        if (separator == std::string::npos)
        {
            std::cerr
                << "配置项格式错误: "
                << line
                << '\n';
            return false;
        }

        const std::string key =
            Trim(line.substr(0, separator));

        const std::string value =
            Trim(line.substr(separator + 1));

        if (!key.empty())
        {
            values[key] = value;
        }
    }

    const auto get_string =
        [&values](
            const std::string& key,
            std::string& output)
        {
            const auto iterator =
                values.find(key);

            if (iterator != values.end())
            {
                output = iterator->second;
            }
        };

    const auto get_int =
        [&values](
            const std::string& key,
            int& output)
        {
            const auto iterator =
                values.find(key);

            if (iterator == values.end())
            {
                return true;
            }

            try
            {
                output = std::stoi(iterator->second);
                return true;
            }
            catch (...)
            {
                std::cerr
                    << "配置项不是有效整数: "
                    << key
                    << '\n';
                return false;
            }
        };

    const auto get_bool =
        [&values](
            const std::string& key,
            bool& output)
        {
            const auto iterator =
                values.find(key);

            if (iterator == values.end())
            {
                return true;
            }

            if (!ParseBool(
                    iterator->second,
                    output))
            {
                std::cerr
                    << "配置项不是有效布尔值: "
                    << key
                    << '\n';
                return false;
            }

            return true;
        };

    get_string("mode", config.mode);
    get_string("local_user_id", config.local_user_id);
    get_string("remote_user_id", config.remote_user_id);
    get_string("room_id", config.room_id);

    /*
     * 兼容原配置文件中的字段名称。
     */
    if (config.local_user_id.empty())
    {
        get_string(
            "user_name",
            config.local_user_id);
    }

    if (config.room_id.empty())
    {
        get_string(
            "meeting_id",
            config.room_id);
    }

    get_string(
        "push_server_url",
        config.push_server_url);

    get_string(
        "pull_server_url",
        config.pull_server_url);

    get_string(
        "app_name",
        config.app_name);

    get_string(
        "rtc_external_address",
        config.rtc_external_address);

    get_string(
        "whip_secret",
        config.whip_secret);

    if (config.whip_secret.empty())
    {
        get_string(
            "publish_secret",
            config.whip_secret);
    }

    get_string(
        "camera_device_id",
        config.camera_device_id);

    get_string(
        "camera_video_format",
        config.camera_video_format);

    get_string(
        "microphone_device_id",
        config.microphone_device_id);

    get_string(
        "speaker_device_id",
        config.speaker_device_id);

    get_string(
        "received_video_file",
        config.received_video_file);

    get_string(
        "received_audio_file",
        config.received_audio_file);

    if (!get_int(
            "sample_rate",
            config.sample_rate) ||
        !get_int(
            "channels",
            config.channels) ||
        !get_int(
            "video_width",
            config.video_width) ||
        !get_int(
            "video_height",
            config.video_height) ||
        !get_int(
            "video_fps",
            config.video_fps) ||
        !get_int(
            "duration_seconds",
            config.duration_seconds) ||
        !get_int(
            "subscribe_retry_seconds",
            config.subscribe_retry_seconds))
    {
        return false;
    }

    if (!get_bool(
            "enable_video",
            config.enable_video) ||
        !get_bool(
            "enable_audio",
            config.enable_audio) ||
        !get_bool(
            "expect_remote_video",
            config.expect_remote_video) ||
        !get_bool(
            "expect_remote_audio",
            config.expect_remote_audio) ||
        !get_bool(
            "save_received_media",
            config.save_received_media))
    {
        return false;
    }

    if (config.local_user_id.empty() ||
        config.remote_user_id.empty() ||
        config.room_id.empty() ||
        config.push_server_url.empty() ||
        config.pull_server_url.empty() ||
        config.app_name.empty() ||
        config.rtc_external_address.empty())
    {
        std::cerr
            << "配置缺少用户、房间或媒体服务器信息\n";
        return false;
    }

    if (config.local_user_id ==
        config.remote_user_id)
    {
        std::cerr
            << "local_user_id和remote_user_id不能相同\n";
        return false;
    }

    if (config.sample_rate <= 0 ||
        config.channels <= 0 ||
        config.video_width <= 0 ||
        config.video_height <= 0 ||
        (config.video_width % 2) != 0 ||
        (config.video_height % 2) != 0 ||
        config.video_fps <= 0 ||
        config.duration_seconds <= 0 ||
        config.subscribe_retry_seconds <= 0)
    {
        std::cerr
            << "音视频参数或测试时长无效\n";
        return false;
    }

    if (config.enable_video &&
        config.camera_video_format.empty())
    {
        std::cerr
            << "camera_video_format不能为空\n";
        return false;
    }

    if (!config.enable_video &&
        !config.enable_audio)
    {
        std::cerr
            << "本地音频和视频不能同时关闭\n";
        return false;
    }

    if (config.received_video_file.empty())
    {
        config.received_video_file =
            config.local_user_id +
            "_received_" +
            config.remote_user_id +
            "_i420.yuv";
    }

    if (config.received_audio_file.empty())
    {
        config.received_audio_file =
            config.local_user_id +
            "_received_" +
            config.remote_user_id +
            "_f32le.pcm";
    }

    return true;
}

const char* TransportStateName(
    VCE::TransportState state)
{
    switch (state)
    {
    case VCE::TransportState::kDisconnected:
        return "Disconnected";

    case VCE::TransportState::kConnecting:
        return "Connecting";

    case VCE::TransportState::kConnected:
        return "Connected";

    case VCE::TransportState::kReconnecting:
        return "Reconnecting";

    case VCE::TransportState::kDisconnecting:
        return "Disconnecting";

    case VCE::TransportState::kFailed:
        return "Failed";

    case VCE::TransportState::kClosed:
        return "Closed";
    }

    return "Unknown";
}

/**
 * 单个端到端媒体测试客户端。
 *
 * 本地链路：
 * CaptureManager -> TransportManager -> SRS WHIP
 *
 * 远端链路：
 * SRS WHEP -> TransportManager -> RenderManager
 */
class VceMediaTest final
    : public VCE::ICaptureDataCallback,
      public VCE::ITransportDataCallback,
      public std::enable_shared_from_this<VceMediaTest>
{
public:
    explicit VceMediaTest(
        MediaTestConfig config)
        : m_config(std::move(config))
    {
    }

    ~VceMediaTest() override
    {
        Shutdown();
    }

    bool Initialize()
    {
        if (!InitializeOpenGLContext())
        {
            return false;
        }

        m_render_manager =
            std::make_shared<VCE::RenderManager>();

        m_transport_manager =
            std::make_shared<VCE::TransportManager>();

        m_capture_manager =
            std::make_shared<VCE::CaptureManager>();

        if (!IsSuccess(
                m_render_manager->Initialize(
                    m_config.sample_rate,
                    m_config.channels,
                    m_config.video_width,
                    m_config.video_height)))
        {
            std::cerr
                << "RenderManager初始化失败\n";
            Shutdown();
            return false;
        }

        VCE::PublishConfig publish_config;
        publish_config.video_width =
            m_config.video_width;
        publish_config.video_height =
            m_config.video_height;
        publish_config.video_fps =
            m_config.video_fps;
        publish_config.audio_sample_rate =
            m_config.sample_rate;
        publish_config.audio_channels =
            m_config.channels;

        if (!IsSuccess(
                m_transport_manager->Initialize(
                    publish_config)))
        {
            std::cerr
                << "TransportManager初始化失败\n";
            Shutdown();
            return false;
        }

        /*
         * OBS初始化需要绑定自己的EGL上下文，
         * 暂时解除当前线程上的GLFW上下文。
         */
        glfwMakeContextCurrent(nullptr);

        const VCE::Result capture_result =
            m_capture_manager->Initialize(
                m_config.sample_rate,
                m_config.channels,
                m_config.video_width,
                m_config.video_height,
                m_config.video_fps);

        /*
         * OBS初始化后重新绑定渲染窗口的OpenGL上下文。
         */
        glfwMakeContextCurrent(m_window);

        if (!IsSuccess(capture_result))
        {
            std::cerr
                << "CaptureManager初始化失败\n";
            Shutdown();
            return false;
        }

        if (!IsSuccess(
                m_capture_manager->
                    SetCaptureDataCallback(
                        shared_from_this())) ||
            !IsSuccess(
                m_transport_manager->
                    SetTransportDataCallback(
                        shared_from_this())))
        {
            std::cerr
                << "媒体回调注册失败\n";
            Shutdown();
            return false;
        }

        if (!ConfigureRoom())
        {
            Shutdown();
            return false;
        }

        glfwMakeContextCurrent(m_window);

        if (!ConfigureRenderUsers() ||
            !ConfigureDevices() ||
            !OpenOutputFiles())
        {
            Shutdown();
            return false;
        }

        m_initialized.store(
            true,
            std::memory_order_release);

        m_accept_callbacks.store(
            true,
            std::memory_order_release);

        PrintConfiguration();
        return true;
    }

    bool Run()
    {
        if (!m_initialized.load(
                std::memory_order_acquire))
        {
            return false;
        }

        if (!StartMedia())
        {
            StopMedia();
            return false;
        }

        const auto start_time =
            std::chrono::steady_clock::now();

        const auto end_time =
            start_time +
            std::chrono::seconds(
                m_config.duration_seconds);

        /*
         * StartMedia已经完成第一次订阅。
         * 主循环必须等到重试间隔到期后再执行下一次订阅，
         * 避免立即取消刚创建的WHEP连接。
         */
        auto next_subscribe_time =
            start_time +
            std::chrono::seconds(
                m_config.subscribe_retry_seconds);

        auto next_av_sync_report_time =
            start_time +
            std::chrono::seconds(1);

        while (!g_stop_requested.load(
                   std::memory_order_acquire) &&
               std::chrono::steady_clock::now() <
                   end_time &&
               !glfwWindowShouldClose(m_window))
        {
            RenderVisibleVideo();

            glfwSwapBuffers(m_window);
            glfwPollEvents();

            const auto now =
                std::chrono::steady_clock::now();

            if (m_config.expect_remote_video &&
                m_config.expect_remote_audio &&
                now >= next_av_sync_report_time)
            {
                PrintAVSyncProgress();

                next_av_sync_report_time =
                    now +
                    std::chrono::seconds(1);
            }

            /*
             * 对方发布尚未建立时周期性重新订阅。
             * 收到任意远端媒体后停止主动重建连接。
             */
            if (!HasReceivedRemoteMedia() &&
                now >= next_subscribe_time)
            {
                RetrySubscription();

                next_subscribe_time =
                    now +
                    std::chrono::seconds(
                        m_config.subscribe_retry_seconds);
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }

        StopMedia();

        const bool passed =
            EvaluateResult();

        PrintResult(passed);
        return passed;
    }

    void Shutdown()
    {
        if (m_shutdown.exchange(
                true,
                std::memory_order_acq_rel))
        {
            return;
        }

        m_accept_callbacks.store(
            false,
            std::memory_order_release);

        if (m_media_started)
        {
            StopMedia();
        }

        if (m_transport_manager)
        {
            m_transport_manager->Uninit();
        }

        /*
         * RenderEngine释放OpenGL资源时，
         * 窗口上下文必须仍然有效。
         */
        if (m_window)
        {
            glfwMakeContextCurrent(m_window);
        }

        if (m_render_manager)
        {
            m_render_manager->Uninit();
        }

        m_render_manager.reset();

        ShutdownOpenGLContext();

        /*
         * GLFW上下文解除后，OBS可以安全清理EGL资源。
         */
        if (m_capture_manager)
        {
            m_capture_manager->Uninit();
        }

        m_capture_manager.reset();
        m_transport_manager.reset();

        {
            std::lock_guard<std::mutex> lock(
                m_video_file_mutex);

            if (m_video_output.is_open())
            {
                m_video_output.close();
            }
        }

        {
            std::lock_guard<std::mutex> lock(
                m_audio_file_mutex);

            if (m_audio_output.is_open())
            {
                m_audio_output.close();
            }
        }

        m_initialized.store(
            false,
            std::memory_order_release);
    }

    void OnCaptureVideoFrame(
        const std::shared_ptr<VCE::I420Frame>& frame) override
    {
        if (!m_accept_callbacks.load(
                std::memory_order_acquire) ||
            !frame ||
            !frame->IsValid())
        {
            return;
        }

        const std::uint64_t frame_index =
            m_captured_video_frames.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

        if (frame_index == 1)
        {
            std::cout
                << "收到第一帧本地视频: "
                << frame->width
                << 'x'
                << frame->height
                << ", timestamp(us)="
                << frame->timestamp_us
                << '\n';
        }

        if (IsSuccess(
                m_render_manager->PushVideoFrame(
                    m_config.local_user_id,
                    frame)))
        {
            m_local_video_render_pushes.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        else
        {
            m_local_video_render_failures.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        if (IsSuccess(
                m_transport_manager->PushVideoFrame(
                    frame,
                    VCE::CaptureType::kCT_Camera)))
        {
            m_published_video_frames.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        else
        {
            m_video_publish_failures.fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }

    void OnCaptureAudioFrame(
        const std::shared_ptr<VCE::AudioFrame>& frame) override
    {
        if (!m_accept_callbacks.load(
                std::memory_order_acquire) ||
            !frame ||
            !frame->IsValid())
        {
            return;
        }

        const std::uint64_t frame_index =
            m_captured_audio_frames.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

        if (frame_index == 1)
        {
            std::cout
                << "收到第一帧本地音频: "
                << frame->samples
                << " samples, "
                << frame->channels
                << " channels, "
                << frame->sample_rate
                << " Hz, timestamp(us)="
                << frame->timestamp_us
                << '\n';
        }

        /*
         * 本地麦克风不送入本地扬声器，
         * 避免产生监听回声。
         */
        if (IsSuccess(
                m_transport_manager->PushAudioFrame(
                    frame)))
        {
            m_published_audio_frames.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        else
        {
            m_audio_publish_failures.fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }

    void OnTransportVideoFrame(
        const std::string& user_id,
        VCE::CaptureType,
        const std::shared_ptr<VCE::I420Frame>& frame) override
    {
        if (!m_accept_callbacks.load(
                std::memory_order_acquire) ||
            user_id != m_config.remote_user_id ||
            !frame ||
            !frame->IsValid())
        {
            return;
        }

        const std::uint64_t frame_index =
            m_received_video_frames.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

        if (RecordRemoteTimestamp(
                frame->timestamp_us,
                m_first_remote_video_timestamp_us,
                m_last_remote_video_timestamp_us,
                m_invalid_remote_video_timestamps,
                m_backward_remote_video_timestamps))
        {
            UpdateRemoteTimelineDifference();
        }

        if (frame_index == 1)
        {
            std::cout
                << "收到第一帧远端视频: user="
                << user_id
                << ", "
                << frame->width
                << 'x'
                << frame->height
                << ", timestamp(us)="
                << frame->timestamp_us
                << '\n';
        }

        /*
         * 实时渲染优先于诊断文件写入。
         */
        if (IsSuccess(
                m_render_manager->PushVideoFrame(
                    user_id,
                    frame)))
        {
            m_remote_video_render_pushes.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        else
        {
            m_remote_video_render_failures.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        SaveVideoFrame(frame);
    }

    void OnTransportAudioFrame(
        const std::string& user_id,
        const std::shared_ptr<VCE::AudioFrame>& frame) override
    {
        if (!m_accept_callbacks.load(
                std::memory_order_acquire) ||
            user_id != m_config.remote_user_id ||
            !frame ||
            !frame->IsValid())
        {
            return;
        }

        const std::uint64_t frame_index =
            m_received_audio_frames.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

        if (RecordRemoteTimestamp(
                frame->timestamp_us,
                m_first_remote_audio_timestamp_us,
                m_last_remote_audio_timestamp_us,
                m_invalid_remote_audio_timestamps,
                m_backward_remote_audio_timestamps))
        {
            UpdateRemoteTimelineDifference();
        }

        if (frame_index == 1)
        {
            std::cout
                << "收到第一帧远端音频: user="
                << user_id
                << ", "
                << frame->samples
                << " samples, "
                << frame->channels
                << " channels, "
                << frame->sample_rate
                << " Hz, timestamp(us)="
                << frame->timestamp_us
                << '\n';
        }

        /*
         * 先送入AudioMixer，再进行可选的诊断文件写入。
         */
        if (IsSuccess(
                m_render_manager->PushAudioFrame(
                    user_id,
                    frame)))
        {
            m_remote_audio_render_pushes.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        else
        {
            m_remote_audio_render_failures.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        SaveAudioFrame(frame);
    }

    void OnTransportConnectionStateChanged(
        VCE::TransportState state) override
    {
        m_transport_state.store(
            state,
            std::memory_order_release);

        std::cout
            << "RTC连接状态: "
            << TransportStateName(state)
            << '\n';
    }

private:
    bool RecordRemoteTimestamp(
        std::int64_t timestamp_us,
        std::atomic<std::int64_t>& first_timestamp,
        std::atomic<std::int64_t>& last_timestamp,
        std::atomic<std::uint64_t>& invalid_count,
        std::atomic<std::uint64_t>& backward_count)
    {
        if (timestamp_us <= 0)
        {
            invalid_count.fetch_add(
                1,
                std::memory_order_relaxed);

            return false;
        }

        std::int64_t empty_timestamp = 0;

        first_timestamp.compare_exchange_strong(
            empty_timestamp,
            timestamp_us,
            std::memory_order_relaxed,
            std::memory_order_relaxed);

        /*
         * last_timestamp表示目前观察到的最大媒体时间。
         * 乱序帧不能使最新时间戳向后退。
         */
        std::int64_t latest_timestamp =
            last_timestamp.load(
                std::memory_order_relaxed);

        while (timestamp_us > latest_timestamp &&
               !last_timestamp.compare_exchange_weak(
                   latest_timestamp,
                   timestamp_us,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed))
        {
        }

        if (latest_timestamp > 0 &&
            timestamp_us < latest_timestamp)
        {
            /*
             * 回退只作为乱序诊断统计，
             * 不直接判定公共时间线失效。
             */
            backward_count.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        return true;
    }

    void UpdateRemoteTimelineDifference()
    {
        const std::int64_t video_timestamp_us =
            m_last_remote_video_timestamp_us.load(
                std::memory_order_relaxed);

        const std::int64_t audio_timestamp_us =
            m_last_remote_audio_timestamp_us.load(
                std::memory_order_relaxed);

        if (video_timestamp_us <= 0 ||
            audio_timestamp_us <= 0)
        {
            return;
        }

        const std::int64_t signed_difference_us =
            video_timestamp_us -
            audio_timestamp_us;

        const std::int64_t absolute_difference_us =
            signed_difference_us >= 0
                ? signed_difference_us
                : -signed_difference_us;

        m_current_remote_av_delta_us.store(
            signed_difference_us,
            std::memory_order_relaxed);

        m_current_remote_av_abs_delta_us.store(
            absolute_difference_us,
            std::memory_order_relaxed);

        UpdateMaximum(
            m_maximum_remote_av_abs_delta_us,
            absolute_difference_us);

        UpdateMinimum(
            m_minimum_remote_av_abs_delta_us,
            absolute_difference_us);

        m_remote_av_timestamp_samples.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    bool HasRemoteTimelineOverlap() const
    {
        const std::int64_t first_video_timestamp_us =
            m_first_remote_video_timestamp_us.load(
                std::memory_order_relaxed);

        const std::int64_t last_video_timestamp_us =
            m_last_remote_video_timestamp_us.load(
                std::memory_order_relaxed);

        const std::int64_t first_audio_timestamp_us =
            m_first_remote_audio_timestamp_us.load(
                std::memory_order_relaxed);

        const std::int64_t last_audio_timestamp_us =
            m_last_remote_audio_timestamp_us.load(
                std::memory_order_relaxed);

        if (first_video_timestamp_us <= 0 ||
            last_video_timestamp_us <= 0 ||
            first_audio_timestamp_us <= 0 ||
            last_audio_timestamp_us <= 0)
        {
            return false;
        }

        const std::int64_t overlap_begin_us =
            std::max(
                first_video_timestamp_us,
                first_audio_timestamp_us);

        const std::int64_t overlap_end_us =
            std::min(
                last_video_timestamp_us,
                last_audio_timestamp_us);

        return overlap_begin_us <= overlap_end_us;
    }

    bool IsRemoteTimelineValid() const
    {
        if (m_remote_av_timestamp_samples.load(
                std::memory_order_relaxed) == 0)
        {
            return false;
        }

        if (m_invalid_remote_video_timestamps.load(
                std::memory_order_relaxed) != 0 ||
            m_invalid_remote_audio_timestamps.load(
                std::memory_order_relaxed) != 0)
        {
            return false;
        }

        if (!HasRemoteTimelineOverlap())
        {
            return false;
        }

        /*
         * 回退计数保留为诊断数据，不作为失败条件。
         * 最新时间差用于判断是否有一路媒体明显停止推进。
         */
        return m_current_remote_av_abs_delta_us.load(
                   std::memory_order_relaxed) <=
               kMaxRemoteTimelineDifferenceUs;
    }

    void PrintAVSyncProgress() const
    {
        if (m_remote_av_timestamp_samples.load(
                std::memory_order_relaxed) == 0)
        {
            std::cout
                << "[AVSync] 等待远端音频和视频时间线同时建立\n";
            return;
        }

        std::cout
            << "[AVSync] video-audio="
            << m_current_remote_av_delta_us.load(
                   std::memory_order_relaxed)
            << " us, abs="
            << m_current_remote_av_abs_delta_us.load(
                   std::memory_order_relaxed)
            << " us, overlap="
            << (HasRemoteTimelineOverlap()
                    ? "yes"
                    : "no")
            << '\n';
    }

    bool InitializeOpenGLContext()
    {
        if (!glfwInit())
        {
            std::cerr
                << "GLFW初始化失败\n";
            return false;
        }

        m_glfw_initialized = true;

        glfwWindowHint(
            GLFW_CONTEXT_VERSION_MAJOR,
            3);

        glfwWindowHint(
            GLFW_CONTEXT_VERSION_MINOR,
            3);

        glfwWindowHint(
            GLFW_OPENGL_PROFILE,
            GLFW_OPENGL_CORE_PROFILE);

        const std::string window_title =
            "VCE Media Test - " +
            m_config.local_user_id;

        m_window = glfwCreateWindow(
            m_config.video_width,
            m_config.video_height,
            window_title.c_str(),
            nullptr,
            nullptr);

        if (!m_window)
        {
            std::cerr
                << "创建GLFW窗口失败\n";

            glfwTerminate();
            m_glfw_initialized = false;
            return false;
        }

        glfwMakeContextCurrent(m_window);
        glfwSwapInterval(1);

        if (!gladLoadGLLoader(
                reinterpret_cast<GLADloadproc>(
                    glfwGetProcAddress)))
        {
            std::cerr
                << "加载OpenGL函数失败\n";

            glfwDestroyWindow(m_window);
            m_window = nullptr;

            glfwTerminate();
            m_glfw_initialized = false;
            return false;
        }

        std::cout
            << "OpenGL上下文初始化成功: "
            << reinterpret_cast<const char*>(
                   glGetString(GL_VERSION))
            << '\n';

        return true;
    }

    void ShutdownOpenGLContext()
    {
        if (m_window)
        {
            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(m_window);
            m_window = nullptr;
        }

        if (m_glfw_initialized)
        {
            glfwTerminate();
            m_glfw_initialized = false;
        }
    }

    bool ConfigureRoom()
    {
        VCE::RoomInfo room_info;
        room_info.local_user_id =
            m_config.local_user_id;
        room_info.room_id =
            m_config.room_id;

        room_info.media_server.push_server_url =
            m_config.push_server_url;

        room_info.media_server.pull_server_url =
            m_config.pull_server_url;

        room_info.media_server.app_name =
            m_config.app_name;

        room_info.media_server.rtc_external_address =
            m_config.rtc_external_address;

        room_info.media_server.publish_secret =
            m_config.whip_secret;

        if (!IsSuccess(
                m_transport_manager->SetRoomInfo(
                    room_info)))
        {
            std::cerr
                << "设置RTC房间信息失败\n";
            return false;
        }

        return true;
    }

    bool ConfigureRenderUsers()
    {
        if (!IsSuccess(
                m_render_manager->AddUser(
                    m_config.local_user_id,
                    true)))
        {
            std::cerr
                << "添加本地渲染用户失败\n";
            return false;
        }

        if (!IsSuccess(
                m_render_manager->AddUser(
                    m_config.remote_user_id,
                    false)))
        {
            std::cerr
                << "添加远端渲染用户失败\n";

            m_render_manager->RemoveUser(
                m_config.local_user_id);

            return false;
        }

        m_render_users_added = true;
        return true;
    }

    bool ConfigureDevices()
    {
        return ConfigureCamera() &&
               ConfigureMicrophone() &&
               ConfigureSpeaker();
    }

    bool ConfigureCamera()
    {
        if (!m_config.enable_video)
        {
            return true;
        }

        std::vector<VCE::CameraDeviceInfo> devices;

        if (!IsSuccess(
                m_capture_manager->GetCameraDevices(
                    devices)))
        {
            std::cerr
                << "枚举摄像头失败\n";
            return false;
        }

        std::cout
            << "\n摄像头设备数量: "
            << devices.size()
            << '\n';

        for (const auto& device : devices)
        {
            std::cout
                << "  名称: "
                << device.name
                << "\n  ID: "
                << device.id
                << '\n';
        }

        if (devices.empty())
        {
            std::cerr
                << "没有可用摄像头\n";
            return false;
        }

        std::string selected_id =
            m_config.camera_device_id;

        if (selected_id.empty())
        {
            selected_id =
                devices.front().id;
        }

        const auto iterator =
            std::find_if(
                devices.begin(),
                devices.end(),
                [&selected_id](
                    const VCE::CameraDeviceInfo& device)
                {
                    return device.id ==
                           selected_id;
                });

        if (iterator == devices.end())
        {
            std::cerr
                << "配置的摄像头不存在: "
                << selected_id
                << '\n';
            return false;
        }

        /*
         * 此处只保存设备ID。
         * 格式和分辨率必须在OpenCamera创建真实Source后设置。
         */
        if (!IsSuccess(
                m_capture_manager->UpdateCameraDevice(
                    selected_id)))
        {
            std::cerr
                << "选择摄像头失败: "
                << selected_id
                << '\n';
            return false;
        }

        std::cout
            << "已选择摄像头: "
            << iterator->name
            << '\n';

        return true;
    }

    bool ConfigureMicrophone()
    {
        if (!m_config.enable_audio)
        {
            return true;
        }

        std::vector<VCE::MicDeviceInfo> devices;

        if (!IsSuccess(
                m_capture_manager->
                    GetMicrophoneDevices(
                        devices)))
        {
            std::cerr
                << "枚举麦克风失败\n";
            return false;
        }

        std::cout
            << "\n麦克风设备数量: "
            << devices.size()
            << '\n';

        for (const auto& device : devices)
        {
            std::cout
                << "  名称: "
                << device.name
                << "\n  ID: "
                << device.id
                << '\n';
        }

        if (devices.empty())
        {
            std::cerr
                << "没有可用麦克风\n";
            return false;
        }

        std::string selected_id =
            m_config.microphone_device_id;

        if (selected_id.empty())
        {
            selected_id =
                devices.front().id;
        }

        const auto iterator =
            std::find_if(
                devices.begin(),
                devices.end(),
                [&selected_id](
                    const VCE::MicDeviceInfo& device)
                {
                    return device.id ==
                           selected_id;
                });

        if (iterator == devices.end())
        {
            std::cerr
                << "配置的麦克风不存在: "
                << selected_id
                << '\n';
            return false;
        }

        if (!IsSuccess(
                m_capture_manager->
                    UpdateMicrophoneDevice(
                        selected_id)))
        {
            std::cerr
                << "选择麦克风失败: "
                << selected_id
                << '\n';
            return false;
        }

        std::cout
            << "已选择麦克风: "
            << iterator->name
            << '\n';

        return true;
    }

    bool ConfigureSpeaker()
    {
        if (!m_config.expect_remote_audio)
        {
            return true;
        }

        std::vector<VCE::SpeakerDeviceInfo> devices;

        if (!IsSuccess(
                m_render_manager->
                    GetAudioSpeakers(
                        devices)))
        {
            std::cerr
                << "枚举扬声器失败\n";
            return false;
        }

        std::cout
            << "\n扬声器设备数量: "
            << devices.size()
            << '\n';

        for (const auto& device : devices)
        {
            std::cout
                << "  名称: "
                << device.name
                << "\n  ID: "
                << device.id
                << "\n  默认: "
                << (device.is_default
                        ? "是"
                        : "否")
                << '\n';
        }

        if (devices.empty())
        {
            std::cerr
                << "没有可用扬声器\n";
            return false;
        }

        if (m_config.speaker_device_id.empty())
        {
            /*
             * AudioRender已经使用系统默认扬声器，
             * 不需要再次重建设备。
             */
            return true;
        }

        const auto iterator =
            std::find_if(
                devices.begin(),
                devices.end(),
                [this](
                    const VCE::SpeakerDeviceInfo& device)
                {
                    return device.id ==
                           m_config.speaker_device_id;
                });

        if (iterator == devices.end())
        {
            std::cerr
                << "配置的扬声器不存在: "
                << m_config.speaker_device_id
                << '\n';
            return false;
        }

        if (!IsSuccess(
                m_render_manager->UpdateAudioSpeaker(
                    m_config.speaker_device_id)))
        {
            std::cerr
                << "选择扬声器失败\n";
            return false;
        }

        std::cout
            << "已选择扬声器: "
            << iterator->name
            << '\n';

        return true;
    }

    bool OpenOutputFiles()
    {
        if (!m_config.save_received_media)
        {
            return true;
        }

        if (m_config.expect_remote_video)
        {
            m_video_output.open(
                m_config.received_video_file,
                std::ios::binary |
                    std::ios::trunc);

            if (!m_video_output.is_open())
            {
                std::cerr
                    << "无法创建远端视频文件: "
                    << m_config.received_video_file
                    << '\n';
                return false;
            }
        }

        if (m_config.expect_remote_audio)
        {
            m_audio_output.open(
                m_config.received_audio_file,
                std::ios::binary |
                    std::ios::trunc);

            if (!m_audio_output.is_open())
            {
                std::cerr
                    << "无法创建远端音频文件: "
                    << m_config.received_audio_file
                    << '\n';
                return false;
            }
        }

        return true;
    }

    bool StartMedia()
    {
        if (m_config.enable_video &&
            !IsSuccess(
                m_transport_manager->
                    StartPublishCameraVideo()))
        {
            std::cerr
                << "启动视频发布失败\n";
            return false;
        }

        if (m_config.enable_audio &&
            !IsSuccess(
                m_transport_manager->
                    StartPublishAudio()))
        {
            std::cerr
                << "启动音频发布失败\n";
            return false;
        }

        /*
         * 初次订阅失败不终止测试，
         * 对方进程可能尚未完成WHIP发布。
         */
        RetrySubscription();

        if (m_config.enable_video)
        {
            if (!IsSuccess(
                    m_capture_manager->OpenCamera()))
            {
                std::cerr
                    << "打开摄像头失败\n";
                return false;
            }

            m_camera_opened = true;

            /*
             * OpenCamera先创建真实摄像头Source并应用设备ID，
             * 随后才能设置该设备的输入格式和分辨率。
             */
            if (!IsSuccess(
                    m_capture_manager->
                        ConfigureCameraInput(
                            m_config.camera_video_format,
                            m_config.video_width,
                            m_config.video_height)))
            {
                std::cerr
                    << "配置摄像头输入失败: format="
                    << m_config.camera_video_format
                    << ", size="
                    << m_config.video_width
                    << 'x'
                    << m_config.video_height
                    << '\n';
                return false;
            }
        }

        if (m_config.enable_audio)
        {
            if (!IsSuccess(
                    m_capture_manager->OpenMic()))
            {
                std::cerr
                    << "打开麦克风失败\n";
                return false;
            }

            m_mic_opened = true;
        }

        m_media_started = true;

        std::cout
            << "\n开始媒体测试，持续 "
            << m_config.duration_seconds
            << " 秒...\n";

        return true;
    }

    void StopMedia()
    {
        if (!m_capture_manager ||
            !m_transport_manager)
        {
            return;
        }

        m_accept_callbacks.store(
            false,
            std::memory_order_release);

        if (m_camera_opened)
        {
            m_capture_manager->CloseCamera();
            m_camera_opened = false;
        }

        if (m_mic_opened)
        {
            m_capture_manager->CloseMic();
            m_mic_opened = false;
        }

        if (m_transport_manager->
                IsUserSubscribedAV(
                    m_config.remote_user_id))
        {
            m_transport_manager->
                UnsubscribeUserAV(
                    m_config.remote_user_id);
        }

        if (m_config.enable_audio &&
            m_transport_manager->
                IsPublishingAudio())
        {
            m_transport_manager->
                StopPublishAudio();
        }

        if (m_config.enable_video &&
            m_transport_manager->
                IsPublishingCameraVideo())
        {
            m_transport_manager->
                StopPublishCameraVideo();
        }

        if (m_render_users_added &&
            m_render_manager)
        {
            m_render_manager->RemoveUser(
                m_config.remote_user_id);

            m_render_manager->RemoveUser(
                m_config.local_user_id);

            m_render_users_added = false;
        }

        m_media_started = false;
    }

    void RetrySubscription()
    {
        if (!m_config.expect_remote_video &&
            !m_config.expect_remote_audio)
        {
            return;
        }

        /*
         * 重试前清理旧会话，确保重新创建PeerConnection。
         * 主循环已保证不会在初次订阅后立即执行这里。
         */
        if (m_transport_manager->
                IsUserSubscribedAV(
                    m_config.remote_user_id))
        {
            m_transport_manager->
                UnsubscribeUserAV(
                    m_config.remote_user_id);
        }

        const std::uint64_t attempt =
            m_subscription_attempts.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

        std::cout
            << "尝试订阅远端用户: "
            << m_config.remote_user_id
            << "，第"
            << attempt
            << "次\n";

        if (!IsSuccess(
                m_transport_manager->
                    SubscribeUserAV(
                        m_config.remote_user_id)))
        {
            std::cout
                << "远端流暂不可用，稍后重试\n";
        }
    }

    bool HasReceivedRemoteMedia() const
    {
        /*
         * 收到任意远端媒体后停止主动重建WHEP连接，
         * 避免已经正常的音频因等待视频被反复断开。
         */
        return m_received_video_frames.load(
                   std::memory_order_relaxed) > 0 ||
               m_received_audio_frames.load(
                   std::memory_order_relaxed) > 0;
    }

    void RenderVisibleVideo()
    {
        /*
         * VideoRender每次绘制都会清空整个Framebuffer。
         * 同一窗口不能依次全屏绘制本地和远端用户。
         *
         * 收到远端视频前显示本地预览；
         * 收到远端视频后只显示远端画面。
         */
        if (m_config.expect_remote_video &&
            m_received_video_frames.load(
                std::memory_order_relaxed) > 0)
        {
            if (IsSuccess(
                    m_render_manager->
                        RenderUserFrame(
                            m_config.remote_user_id)))
            {
                m_remote_render_calls.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            else
            {
                m_remote_render_failures.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }

            return;
        }

        if (!m_config.enable_video ||
            m_captured_video_frames.load(
                std::memory_order_relaxed) == 0)
        {
            return;
        }

        if (IsSuccess(
                m_render_manager->
                    RenderUserFrame(
                        m_config.local_user_id)))
        {
            m_local_render_calls.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        else
        {
            m_local_render_failures.fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }

    void SaveVideoFrame(
        const std::shared_ptr<VCE::I420Frame>& frame)
    {
        if (!m_config.save_received_media ||
            !m_video_output.is_open() ||
            !frame ||
            !frame->IsValid())
        {
            return;
        }

        const std::size_t y_size =
            frame->GetYPlaneSize();

        const std::size_t uv_size =
            frame->GetUVPlaneSize();

        std::lock_guard<std::mutex> lock(
            m_video_file_mutex);

        m_video_output.write(
            reinterpret_cast<const char*>(
                frame->data[0].get()),
            static_cast<std::streamsize>(
                y_size));

        m_video_output.write(
            reinterpret_cast<const char*>(
                frame->data[1].get()),
            static_cast<std::streamsize>(
                uv_size));

        m_video_output.write(
            reinterpret_cast<const char*>(
                frame->data[2].get()),
            static_cast<std::streamsize>(
                uv_size));
    }

    void SaveAudioFrame(
        const std::shared_ptr<VCE::AudioFrame>& frame)
    {
        if (!m_config.save_received_media ||
            !m_audio_output.is_open() ||
            !frame ||
            !frame->IsValid())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(
            m_audio_file_mutex);

        m_audio_output.write(
            reinterpret_cast<const char*>(
                frame->data.get()),
            static_cast<std::streamsize>(
                frame->GetDataSize()));
    }

    bool EvaluateResult() const
    {
        bool passed = true;

        if (m_config.enable_video)
        {
            passed =
                passed &&
                m_captured_video_frames.load() > 0 &&
                m_published_video_frames.load() > 0 &&
                m_local_video_render_pushes.load() > 0 &&
                m_video_publish_failures.load() == 0 &&
                m_local_video_render_failures.load() == 0;
        }

        if (m_config.enable_audio)
        {
            passed =
                passed &&
                m_captured_audio_frames.load() > 0 &&
                m_published_audio_frames.load() > 0 &&
                m_audio_publish_failures.load() == 0;
        }

        if (m_config.expect_remote_video)
        {
            passed =
                passed &&
                m_received_video_frames.load() > 0 &&
                m_remote_video_render_pushes.load() > 0 &&
                m_remote_render_calls.load() > 0 &&
                m_invalid_remote_video_timestamps.load() == 0 &&
                m_remote_video_render_failures.load() == 0 &&
                m_remote_render_failures.load() == 0;
        }

        if (m_config.expect_remote_audio)
        {
            passed =
                passed &&
                m_received_audio_frames.load() > 0 &&
                m_remote_audio_render_pushes.load() > 0 &&
                m_invalid_remote_audio_timestamps.load() == 0 &&
                m_remote_audio_render_failures.load() == 0;
        }

        if (m_config.expect_remote_video &&
            m_config.expect_remote_audio)
        {
            passed =
                passed &&
                IsRemoteTimelineValid();
        }

        return passed;
    }

    void PrintConfiguration() const
    {
        std::cout
            << "\n========== VCE媒体测试配置 ==========\n"
            << "运行模式: "
            << m_config.mode
            << '\n'
            << "本地用户: "
            << m_config.local_user_id
            << '\n'
            << "远端用户: "
            << m_config.remote_user_id
            << '\n'
            << "媒体房间: "
            << m_config.room_id
            << '\n'
            << "WHIP地址: "
            << m_config.push_server_url
            << '\n'
            << "WHEP地址: "
            << m_config.pull_server_url
            << '\n'
            << "RTC公网地址: "
            << m_config.rtc_external_address
            << '\n'
            << "摄像头输入格式: "
            << m_config.camera_video_format
            << '\n'
            << "视频配置: "
            << m_config.video_width
            << 'x'
            << m_config.video_height
            << '@'
            << m_config.video_fps
            << "fps\n"
            << "音频配置: "
            << m_config.sample_rate
            << "Hz/"
            << m_config.channels
            << "ch\n"
            << "测试时长: "
            << m_config.duration_seconds
            << "秒\n"
            << "保存接收媒体: "
            << (m_config.save_received_media
                    ? "是"
                    : "否")
            << '\n';
    }

    void PrintResult(bool passed) const
    {
        std::cout
            << "\n========== VCE媒体测试结果 ==========\n"
            << "本地视频采集帧: "
            << m_captured_video_frames.load()
            << '\n'
            << "本地视频发布成功: "
            << m_published_video_frames.load()
            << '\n'
            << "本地视频发布失败: "
            << m_video_publish_failures.load()
            << '\n'
            << "本地音频采集帧: "
            << m_captured_audio_frames.load()
            << '\n'
            << "本地音频发布成功: "
            << m_published_audio_frames.load()
            << '\n'
            << "本地音频发布失败: "
            << m_audio_publish_failures.load()
            << '\n'
            << "远端视频接收帧: "
            << m_received_video_frames.load()
            << '\n'
            << "远端音频接收帧: "
            << m_received_audio_frames.load()
            << '\n'
            << "本地视频渲染次数: "
            << m_local_render_calls.load()
            << '\n'
            << "远端视频渲染次数: "
            << m_remote_render_calls.load()
            << '\n'
            << "本地视频推送失败: "
            << m_local_video_render_failures.load()
            << '\n'
            << "远端视频推送失败: "
            << m_remote_video_render_failures.load()
            << '\n'
            << "远端音频推送失败: "
            << m_remote_audio_render_failures.load()
            << '\n'
            << "远端视频渲染失败: "
            << m_remote_render_failures.load()
            << '\n'
            << "订阅尝试次数: "
            << m_subscription_attempts.load()
            << '\n'
            << "远端视频无效时间戳: "
            << m_invalid_remote_video_timestamps.load()
            << '\n'
            << "远端音频无效时间戳: "
            << m_invalid_remote_audio_timestamps.load()
            << '\n'
            << "远端视频时间戳回退: "
            << m_backward_remote_video_timestamps.load()
            << '\n'
            << "远端音频时间戳回退: "
            << m_backward_remote_audio_timestamps.load()
            << '\n'
            << "远端时间线是否重叠: "
            << (HasRemoteTimelineOverlap()
                    ? "是"
                    : "否")
            << '\n'
            << "最新video-audio差值: "
            << m_current_remote_av_delta_us.load()
            << " us\n"
            << "最新绝对差值: "
            << m_current_remote_av_abs_delta_us.load()
            << " us\n"
            << "历史最大绝对差值: "
            << m_maximum_remote_av_abs_delta_us.load()
            << " us\n"
            << "同步时间线检查: "
            << (m_config.expect_remote_video &&
                    m_config.expect_remote_audio
                    ? (IsRemoteTimelineValid()
                           ? "通过"
                           : "失败")
                    : "未启用")
            << '\n'
            << "最终结果: "
            << (passed
                    ? "通过"
                    : "失败")
            << '\n';

        if (m_config.save_received_media)
        {
            std::cout
                << "远端视频文件: "
                << m_config.received_video_file
                << '\n'
                << "远端音频文件: "
                << m_config.received_audio_file
                << '\n';
        }
    }

private:
    MediaTestConfig m_config;

    std::shared_ptr<VCE::CaptureManager>
        m_capture_manager;

    std::shared_ptr<VCE::RenderManager>
        m_render_manager;

    std::shared_ptr<VCE::TransportManager>
        m_transport_manager;

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_accept_callbacks{false};
    std::atomic<bool> m_shutdown{false};

    bool m_media_started{false};
    bool m_camera_opened{false};
    bool m_mic_opened{false};
    bool m_render_users_added{false};

    std::atomic<VCE::TransportState>
        m_transport_state{
            VCE::TransportState::kDisconnected};

    std::atomic<std::uint64_t>
        m_captured_video_frames{0};

    std::atomic<std::uint64_t>
        m_captured_audio_frames{0};

    std::atomic<std::uint64_t>
        m_published_video_frames{0};

    std::atomic<std::uint64_t>
        m_published_audio_frames{0};

    std::atomic<std::uint64_t>
        m_video_publish_failures{0};

    std::atomic<std::uint64_t>
        m_audio_publish_failures{0};

    std::atomic<std::uint64_t>
        m_received_video_frames{0};

    std::atomic<std::uint64_t>
        m_received_audio_frames{0};

    /*
     * 远端解码媒体公共时间线统计。
     * 这些成员只用于端到端诊断，不参与生产同步决策。
     */
    std::atomic<std::int64_t>
        m_first_remote_video_timestamp_us{0};

    std::atomic<std::int64_t>
        m_last_remote_video_timestamp_us{0};

    std::atomic<std::int64_t>
        m_first_remote_audio_timestamp_us{0};

    std::atomic<std::int64_t>
        m_last_remote_audio_timestamp_us{0};

    std::atomic<std::uint64_t>
        m_invalid_remote_video_timestamps{0};

    std::atomic<std::uint64_t>
        m_invalid_remote_audio_timestamps{0};

    std::atomic<std::uint64_t>
        m_backward_remote_video_timestamps{0};

    std::atomic<std::uint64_t>
        m_backward_remote_audio_timestamps{0};

    std::atomic<std::uint64_t>
        m_remote_av_timestamp_samples{0};

    std::atomic<std::int64_t>
        m_current_remote_av_delta_us{0};

    std::atomic<std::int64_t>
        m_current_remote_av_abs_delta_us{0};

    std::atomic<std::int64_t>
        m_maximum_remote_av_abs_delta_us{0};

    std::atomic<std::int64_t>
        m_minimum_remote_av_abs_delta_us{
            std::numeric_limits<std::int64_t>::max()};

    std::atomic<std::uint64_t>
        m_local_video_render_pushes{0};

    std::atomic<std::uint64_t>
        m_remote_video_render_pushes{0};

    std::atomic<std::uint64_t>
        m_remote_audio_render_pushes{0};

    std::atomic<std::uint64_t>
        m_local_video_render_failures{0};

    std::atomic<std::uint64_t>
        m_remote_video_render_failures{0};

    std::atomic<std::uint64_t>
        m_remote_audio_render_failures{0};

    std::atomic<std::uint64_t>
        m_local_render_calls{0};

    std::atomic<std::uint64_t>
        m_remote_render_calls{0};

    std::atomic<std::uint64_t>
        m_local_render_failures{0};

    std::atomic<std::uint64_t>
        m_remote_render_failures{0};

    std::atomic<std::uint64_t>
        m_subscription_attempts{0};

    std::ofstream m_video_output;
    std::ofstream m_audio_output;

    std::mutex m_video_file_mutex;
    std::mutex m_audio_file_mutex;

    GLFWwindow* m_window{nullptr};
    bool m_glfw_initialized{false};
};

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr
            << "用法: "
            << argv[0]
            << " <config-file>\n";
        return 1;
    }

    std::signal(
        SIGINT,
        SignalHandler);

    std::signal(
        SIGTERM,
        SignalHandler);

    MediaTestConfig config;

    if (!LoadConfig(
            argv[1],
            config))
    {
        return 2;
    }

    auto test =
        std::make_shared<VceMediaTest>(
            std::move(config));

    if (!test->Initialize())
    {
        std::cerr
            << "VCE媒体测试初始化失败\n";
        return 3;
    }

    const bool passed =
        test->Run();

    test->Shutdown();

    return passed ? 0 : 4;
}