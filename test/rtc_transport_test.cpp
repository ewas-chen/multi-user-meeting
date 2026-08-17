#include "transport/include/ITransportEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace TRANSPORT;

constexpr int kVideoWidth = 640;
constexpr int kVideoHeight = 480;
constexpr int kVideoFps = 30;

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioFrameSamples = 960; // 48kHz下20ms音频

constexpr const char* kReceivedVideoFile =
    "rtc_received_640x480_i420.yuv";

constexpr const char* kReceivedAudioFile =
    "rtc_received_48000hz_2ch_f32le.pcm";

struct RtcTestConfig {
    TransportTargetRoomInfo room_info;
    std::string subscribe_user_id;
    std::string video_file;
    std::string audio_file;
    int duration_seconds{30};
};

struct RtcTestContext {
    std::mutex state_mutex;
    std::condition_variable state_cv;
    ConnectionState connection_state{ConnectionState::kDisconnected};
    bool connected{false};
    bool connection_failed{false};

    std::mutex video_mutex;
    std::mutex audio_mutex;
    std::ofstream video_output;
    std::ofstream audio_output;

    std::int64_t last_video_timestamp{-1};
    std::int64_t last_audio_timestamp{-1};

    std::atomic<std::size_t> pushed_video_frames{0};
    std::atomic<std::size_t> pushed_audio_frames{0};
    std::atomic<std::size_t> failed_video_pushes{0};
    std::atomic<std::size_t> failed_audio_pushes{0};

    std::atomic<std::size_t> received_video_frames{0};
    std::atomic<std::size_t> received_audio_frames{0};
    std::atomic<std::size_t> invalid_video_frames{0};
    std::atomic<std::size_t> invalid_audio_frames{0};
};

/**
 * @brief 去除配置项首尾空白字符
 */
std::string Trim(std::string value)
{
    const auto first = std::find_if_not(
        value.begin(), value.end(),
        [](unsigned char character) {
            return std::isspace(character);
        });

    const auto last = std::find_if_not(
        value.rbegin(), value.rend(),
        [](unsigned char character) {
            return std::isspace(character);
        }).base();

    if (first >= last) {
        return {};
    }

    return std::string(first, last);
}

/**
 * @brief 从key=value配置文件读取RTC测试参数
 */
bool LoadRtcTestConfig(
    const std::string& file_name,
    RtcTestConfig& config)
{
    std::ifstream input(file_name);
    if (!input) {
        std::cerr << "无法打开RTC配置文件: "
                  << file_name << '\n';
        return false;
    }

    std::string line;

    while (std::getline(input, line)) {
        const std::size_t comment_position = line.find('#');
        if (comment_position != std::string::npos) {
            line.erase(comment_position);
        }

        line = Trim(std::move(line));
        if (line.empty()) {
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            std::cerr << "无效配置行: " << line << '\n';
            return false;
        }

        const std::string key = Trim(line.substr(0, separator));
        const std::string value = Trim(line.substr(separator + 1));

        if (key == "push_server_url") {
            config.room_info.push_server_url = value;
        } else if (key == "pull_server_url") {
            config.room_info.pull_server_url = value;
        } else if (key == "room_id") {
            config.room_info.room_id = value;
        } else if (key == "local_user_id") {
            config.room_info.local_user_id = value;
        } else if (key == "subscribe_user_id") {
            config.subscribe_user_id = value;
        } else if (key == "whip_secret") {
            config.room_info.whip_secret = value;
        } else if (key == "rtc_external_address") {
            config.room_info.rtc_external_address = value;
        } else if (key == "video_file") {
            config.video_file = value;
        } else if (key == "audio_file") {
            config.audio_file = value;
        } else if (key == "duration_seconds") {
            try {
                config.duration_seconds = std::stoi(value);
            } catch (...) {
                std::cerr << "duration_seconds配置无效\n";
                return false;
            }
        }
    }

    const bool valid =
        !config.room_info.push_server_url.empty() &&
        !config.room_info.pull_server_url.empty() &&
        !config.room_info.room_id.empty() &&
        !config.room_info.local_user_id.empty() &&
        !config.room_info.whip_secret.empty() &&
        !config.room_info.rtc_external_address.empty() &&
        !config.subscribe_user_id.empty() &&
        !config.video_file.empty() &&
        !config.audio_file.empty() &&
        config.duration_seconds > 0;

    if (!valid) {
        std::cerr << "RTC配置文件缺少必要配置\n";
    }

    return valid;
}

/**
 * @brief 将连接状态转换为便于输出的文本
 */
const char* ConnectionStateToString(ConnectionState state)
{
    switch (state) {
    case ConnectionState::kDisconnected:
        return "Disconnected";
    case ConnectionState::kConnecting:
        return "Connecting";
    case ConnectionState::kConnected:
        return "Connected";
    case ConnectionState::kDisconnecting:
        return "Disconnecting";
    case ConnectionState::kFailed:
        return "Failed";
    case ConnectionState::kClosed:
        return "Closed";
    }

    return "Unknown";
}

/**
 * @brief 注册连接状态和WHEP接收帧回调
 *
 * 接收到的I420和Float32 PCM会保存到本地文件。
 */
void RegisterTransportCallbacks(
    ITransportEngine& engine,
    RtcTestContext& context,
    const std::string& expected_user_id)
{
    engine.RegisterConnectionStateCallback(
        [&context](ConnectionState state) {
            {
                std::lock_guard<std::mutex> lock(
                    context.state_mutex);

                context.connection_state = state;

                if (state == ConnectionState::kConnected) {
                    context.connected = true;
                } else if (state == ConnectionState::kFailed) {
                    context.connection_failed = true;
                }
            }

            std::cout << "RTC连接状态: "
                      << ConnectionStateToString(state)
                      << '\n';

            context.state_cv.notify_all();
        });

    engine.RegisterVideoCallback(
        [&context, expected_user_id](
            const std::string& user_id,
            const std::shared_ptr<I420Frame>& frame) {
            std::lock_guard<std::mutex> lock(
                context.video_mutex);

            if (user_id != expected_user_id ||
                !frame ||
                !frame->IsValid() ||
                frame->width != kVideoWidth ||
                frame->height != kVideoHeight) {
                ++context.invalid_video_frames;
                return;
            }

            if (context.last_video_timestamp >= 0 &&
                frame->timestamp_us <
                context.last_video_timestamp) {
                ++context.invalid_video_frames;
                return;
            }

            context.last_video_timestamp =
                frame->timestamp_us;

            const std::size_t y_size =
                static_cast<std::size_t>(frame->width) *
                frame->height;

            const std::size_t uv_size =
                static_cast<std::size_t>(frame->width / 2) *
                (frame->height / 2);

            context.video_output.write(
                reinterpret_cast<const char*>(
                    frame->data[0].get()),
                static_cast<std::streamsize>(y_size));

            context.video_output.write(
                reinterpret_cast<const char*>(
                    frame->data[1].get()),
                static_cast<std::streamsize>(uv_size));

            context.video_output.write(
                reinterpret_cast<const char*>(
                    frame->data[2].get()),
                static_cast<std::streamsize>(uv_size));

            if (!context.video_output) {
                ++context.invalid_video_frames;
                return;
            }

            const std::size_t frame_number =
                ++context.received_video_frames;

            if (frame_number == 1) {
                std::cout
                    << "收到第一帧WHEP视频: "
                    << frame->width << 'x'
                    << frame->height
                    << ", timestamp(us)="
                    << frame->timestamp_us
                    << '\n';
            }
        });

    engine.RegisterAudioCallback(
        [&context, expected_user_id](
            const std::string& user_id,
            const std::shared_ptr<AudioFrame>& frame) {
            std::lock_guard<std::mutex> lock(
                context.audio_mutex);

            if (user_id != expected_user_id ||
                !frame ||
                !frame->IsValid() ||
                frame->sample_rate != kAudioSampleRate ||
                frame->channels != kAudioChannels) {
                ++context.invalid_audio_frames;
                return;
            }

            if (context.last_audio_timestamp >= 0 &&
                frame->timestamp_us <
                context.last_audio_timestamp) {
                ++context.invalid_audio_frames;
                return;
            }

            context.last_audio_timestamp =
                frame->timestamp_us;

            const std::size_t data_size =
                static_cast<std::size_t>(frame->samples) *
                frame->channels * sizeof(float);

            context.audio_output.write(
                reinterpret_cast<const char*>(
                    frame->data.get()),
                static_cast<std::streamsize>(data_size));

            if (!context.audio_output) {
                ++context.invalid_audio_frames;
                return;
            }

            const std::size_t frame_number =
                ++context.received_audio_frames;

            if (frame_number == 1) {
                std::cout
                    << "收到第一帧WHEP音频: "
                    << frame->samples
                    << " samples, "
                    << frame->channels
                    << " channels, timestamp(us)="
                    << frame->timestamp_us
                    << '\n';
            }
        });
}

/**
 * @brief 等待WHIP PeerConnection连接成功
 */
bool WaitForConnected(
    RtcTestContext& context,
    std::chrono::seconds timeout)
{
    std::unique_lock<std::mutex> lock(context.state_mutex);

    const bool state_changed = context.state_cv.wait_for(
        lock, timeout,
        [&context] {
            return context.connected ||
                   context.connection_failed;
        });

    return state_changed &&
           context.connected &&
           !context.connection_failed;
}

/**
 * @brief 从测试文件读取一帧I420视频
 */
std::shared_ptr<I420Frame> ReadVideoFrame(
    std::ifstream& input,
    std::size_t frame_index)
{
    const std::size_t y_size =
        static_cast<std::size_t>(kVideoWidth) *
        kVideoHeight;

    const std::size_t uv_size =
        static_cast<std::size_t>(kVideoWidth / 2) *
        (kVideoHeight / 2);

    auto frame = std::make_shared<I420Frame>();
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

    if (!input) {
        return nullptr;
    }

    frame->width = kVideoWidth;
    frame->height = kVideoHeight;
    frame->timestamp_us =
        static_cast<std::int64_t>(frame_index) *
        1000000LL / kVideoFps;

    return frame;
}

/**
 * @brief 从测试文件读取一帧20ms Float32 PCM
 */
std::shared_ptr<AudioFrame> ReadAudioFrame(
    std::ifstream& input,
    std::size_t frame_index)
{
    const std::size_t data_size =
        static_cast<std::size_t>(kAudioFrameSamples) *
        kAudioChannels * sizeof(float);

    auto frame = std::make_shared<AudioFrame>();
    frame->data = std::shared_ptr<std::uint8_t[]>(
        new std::uint8_t[data_size]);

    input.read(
        reinterpret_cast<char*>(frame->data.get()),
        static_cast<std::streamsize>(data_size));

    if (!input) {
        return nullptr;
    }

    frame->samples = kAudioFrameSamples;
    frame->channels = kAudioChannels;
    frame->sample_rate = kAudioSampleRate;
    frame->timestamp_us =
        static_cast<std::int64_t>(frame_index) *
        kAudioFrameSamples * 1000000LL /
        kAudioSampleRate;

    return frame;
}

/**
 * @brief 按30 FPS读取并推送I420视频
 */
void PublishVideo(
    ITransportEngine& engine,
    const RtcTestConfig& config,
    RtcTestContext& context,
    std::chrono::steady_clock::time_point start_time)
{
    std::ifstream input(config.video_file, std::ios::binary);
    if (!input) {
        std::cerr << "无法打开视频输入文件: "
                  << config.video_file << '\n';
        ++context.failed_video_pushes;
        return;
    }

    const std::size_t max_frames =
        static_cast<std::size_t>(
            config.duration_seconds * kVideoFps);

    const auto frame_interval =
        std::chrono::microseconds(1000000 / kVideoFps);

    for (std::size_t index = 0;
         index < max_frames;
         ++index) {
        auto frame = ReadVideoFrame(input, index);
        if (!frame) {
            break;
        }

        std::this_thread::sleep_until(
            start_time + frame_interval * index);

        if (engine.PushVideoFrame(frame)) {
            ++context.pushed_video_frames;
        } else {
            ++context.failed_video_pushes;
        }
    }
}

/**
 * @brief 按20ms间隔读取并推送Float32 PCM
 */
void PublishAudio(
    ITransportEngine& engine,
    const RtcTestConfig& config,
    RtcTestContext& context,
    std::chrono::steady_clock::time_point start_time)
{
    std::ifstream input(config.audio_file, std::ios::binary);
    if (!input) {
        std::cerr << "无法打开音频输入文件: "
                  << config.audio_file << '\n';
        ++context.failed_audio_pushes;
        return;
    }

    const std::size_t max_frames =
        static_cast<std::size_t>(
            config.duration_seconds * 1000 / 20);

    const auto frame_interval =
        std::chrono::milliseconds(20);

    for (std::size_t index = 0;
         index < max_frames;
         ++index) {
        auto frame = ReadAudioFrame(input, index);
        if (!frame) {
            break;
        }

        std::this_thread::sleep_until(
            start_time + frame_interval * index);

        if (engine.PushAudioFrame(frame)) {
            ++context.pushed_audio_frames;
        } else {
            ++context.failed_audio_pushes;
        }
    }
}

/**
 * @brief 同时按照真实采集节奏推送音频和视频
 */
void RunMediaPublishTest(
    ITransportEngine& engine,
    const RtcTestConfig& config,
    RtcTestContext& context)
{
    const auto start_time =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(100);

    std::thread video_thread(
        PublishVideo,
        std::ref(engine),
        std::cref(config),
        std::ref(context),
        start_time);

    std::thread audio_thread(
        PublishAudio,
        std::ref(engine),
        std::cref(config),
        std::ref(context),
        start_time);

    video_thread.join();
    audio_thread.join();
}

/**
 * @brief 输出RTC回环测试结果
 */
bool PrintTestResult(const RtcTestContext& context)
{
    const bool passed =
        context.connected &&
        !context.connection_failed &&
        context.pushed_video_frames > 0 &&
        context.pushed_audio_frames > 0 &&
        context.failed_video_pushes == 0 &&
        context.failed_audio_pushes == 0 &&
        context.received_video_frames > 0 &&
        context.received_audio_frames > 0 &&
        context.invalid_video_frames == 0 &&
        context.invalid_audio_frames == 0;

    std::cout << "\n========== RTC传输测试结果 ==========\n";
    std::cout << "成功推送视频帧: "
              << context.pushed_video_frames << '\n';
    std::cout << "视频推送失败数: "
              << context.failed_video_pushes << '\n';
    std::cout << "成功推送音频帧: "
              << context.pushed_audio_frames << '\n';
    std::cout << "音频推送失败数: "
              << context.failed_audio_pushes << '\n';

    std::cout << "WHEP接收视频帧: "
              << context.received_video_frames << '\n';
    std::cout << "无效接收视频帧: "
              << context.invalid_video_frames << '\n';
    std::cout << "WHEP接收音频帧: "
              << context.received_audio_frames << '\n';
    std::cout << "无效接收音频帧: "
              << context.invalid_audio_frames << '\n';

    std::cout << "视频输出文件: "
              << kReceivedVideoFile << '\n';
    std::cout << "音频输出文件: "
              << kReceivedAudioFile << '\n';
    std::cout << "最终结果: "
              << (passed ? "通过" : "失败") << '\n';

    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr
            << "用法: ./rtc_transport_test "
            << "<rtc_test.conf>\n";
        return 1;
    }

    RtcTestConfig config;
    if (!LoadRtcTestConfig(argv[1], config)) {
        return 1;
    }

    std::cout << "========== RTC WHIP/WHEP回环测试 ==========\n";
    std::cout << "WHIP地址: "
              << config.room_info.push_server_url << '\n';
    std::cout << "WHEP地址: "
              << config.room_info.pull_server_url << '\n';
    std::cout << "房间ID: "
              << config.room_info.room_id << '\n';
    std::cout << "发布用户: "
              << config.room_info.local_user_id << '\n';
    std::cout << "订阅用户: "
              << config.subscribe_user_id << '\n';
    std::cout << "RTC公网地址: "
              << config.room_info.rtc_external_address << '\n';
    std::cout << "测试时长: "
              << config.duration_seconds << "秒\n";

    RtcTestContext context;
    context.video_output.open(
        kReceivedVideoFile,
        std::ios::binary);

    context.audio_output.open(
        kReceivedAudioFile,
        std::ios::binary);

    if (!context.video_output || !context.audio_output) {
        std::cerr << "创建RTC接收文件失败\n";
        return 1;
    }

    auto engine = ITransportEngine::Create();
    if (!engine) {
        std::cerr << "创建TransportEngine失败\n";
        return 1;
    }

    PublishInfo publish_info;
    publish_info.video_width = kVideoWidth;
    publish_info.video_height = kVideoHeight;
    publish_info.video_fps = kVideoFps;
    publish_info.audio_sample_rate = kAudioSampleRate;
    publish_info.audio_channels = kAudioChannels;

    if (!engine->Initialize(publish_info)) {
        std::cerr << "TransportEngine初始化失败\n";
        return 1;
    }

    engine->SetTargetRoomInfo(config.room_info);

    RegisterTransportCallbacks(
        *engine,
        context,
        config.subscribe_user_id);

    if (!engine->StartPublishVideo()) {
        std::cerr << "启动视频发布失败\n";
        engine->Uninit();
        return 1;
    }

    if (!engine->StartPublishAudio()) {
        std::cerr << "启动音频发布失败\n";
        engine->StopPublishVideo();
        engine->Uninit();
        return 1;
    }

    std::cout << "等待WHIP连接建立...\n";

    if (!WaitForConnected(context, std::chrono::seconds(15))) {
        std::cerr << "WHIP连接超时或连接失败\n";
        engine->StopPublishAudio();
        engine->StopPublishVideo();
        engine->Uninit();
        return 1;
    }

    std::cout << "WHIP连接成功\n";

    if (!engine->SubscribeUserAV(
            config.subscribe_user_id)) {
        std::cerr << "启动WHEP订阅失败\n";
        engine->StopPublishAudio();
        engine->StopPublishVideo();
        engine->Uninit();
        return 1;
    }

    if (!engine->IsUserSubscribedAV(
            config.subscribe_user_id)) {
        std::cerr << "订阅状态检查失败\n";
        engine->UnsubscribeUserAV(
            config.subscribe_user_id);
        engine->StopPublishAudio();
        engine->StopPublishVideo();
        engine->Uninit();
        return 1;
    }

    /*
     * 给WHEP信令和ICE连接留出少量时间。
     * 后续可以增加每个订阅用户独立的连接状态回调。
     */
    std::this_thread::sleep_for(
        std::chrono::milliseconds(800));

    std::cout << "开始推送测试音视频...\n";

    RunMediaPublishTest(*engine, config, context);

    /*
     * 等待网络和解码队列输出剩余帧。
     */
    std::this_thread::sleep_for(
        std::chrono::seconds(2));

    const bool passed = PrintTestResult(context);

    const bool unsubscribe_result =
        engine->UnsubscribeUserAV(
            config.subscribe_user_id);

    const bool stop_audio_result =
        engine->StopPublishAudio();

    const bool stop_video_result =
        engine->StopPublishVideo();

    std::cout << "取消订阅: "
              << (unsubscribe_result ? "成功" : "失败")
              << '\n';

    std::cout << "停止音频发布: "
              << (stop_audio_result ? "成功" : "失败")
              << '\n';

    std::cout << "停止视频发布: "
              << (stop_video_result ? "成功" : "失败")
              << '\n';
    std::cout << "丢帧: "
              << m_video_queue_drop_count
              << '\n';

    engine->Uninit();

    context.video_output.close();
    context.audio_output.close();

    std::cout << "TransportEngine反初始化完成\n";

    return passed ? 0 : 1;
}