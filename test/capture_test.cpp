#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ICameraSource.h"
#include "ICameraSourceProperty.h"
#include "ICaptureEngine.h"
#include "IMicSource.h"
#include "IMicSourceProperty.h"

namespace
{

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kOutputWidth = 640;
constexpr int kOutputHeight = 480;
constexpr int kFps = 30;

// 使用YUYV，避免部分USB摄像头的MJPEG解码异常。
constexpr const char* kTargetVideoFormat = "YUYV";

/*
 * 留空时使用枚举到的第一个摄像头。
 * 如果机器存在多个摄像头，建议填写"/dev/video0"或"/dev/video2"。
 */
constexpr const char* kTargetCameraDeviceId = "";

/*
 * 必须根据运行机器修改。
 * 如果找不到该设备，测试会直接报告失败，不会误用默认麦克风。
 */
constexpr const char* kTargetMicDeviceId =
    "alsa_input.usb-Generic_USB2.0_Device_20170726905926-00.mono-fallback";

/*
 * 使用60秒验证摄像头能否越过原来的30秒边界持续采集。
 * 原来的30秒测试文件在30秒处结束，ffplay会停留在最后一帧。
 */
constexpr auto kCaptureDuration = std::chrono::seconds(60);

constexpr const char* kCameraSourceName = "capture_test_camera";
constexpr const char* kMicSourceName = "capture_test_microphone";

const std::string kVideoFileName =
    "/home/ewas/VC_code/meeting-client/source/camera_" +
    std::to_string(kOutputWidth) + "x" +
    std::to_string(kOutputHeight) + "_i420.yuv";

constexpr const char* kAudioFileName =
    "/home/ewas/VC_code/meeting-client/source/"
    "microphone_48000hz_2ch_f32le.pcm";

struct CaptureStatistics
{
    std::atomic<std::uint64_t> video_frames{0};
    std::atomic<std::uint64_t> saved_video_frames{0};
    std::atomic<std::uint64_t> audio_frames{0};
    std::atomic<std::uint64_t> audio_samples{0};

    std::atomic<std::uint64_t> invalid_video_frames{0};
    std::atomic<std::uint64_t> invalid_audio_frames{0};
    std::atomic<std::uint64_t> video_format_errors{0};
    std::atomic<std::uint64_t> audio_format_errors{0};

    std::atomic<std::uint64_t> video_timestamp_regressions{0};
    std::atomic<std::uint64_t> audio_timestamp_regressions{0};

    std::atomic<std::uint64_t> video_bytes{0};
    std::atomic<std::uint64_t> audio_bytes{0};

    std::atomic<std::int64_t> last_video_timestamp_us{-1};
    std::atomic<std::int64_t> last_audio_timestamp_us{-1};

    /*
     * OBS场景在摄像头停止后仍可能继续输出重复的最后一帧。
     * 记录连续重复帧，用于识别这种“回调正常但摄像头已经卡死”的情况。
     */
    std::atomic<std::uint64_t> last_video_hash{0};
    std::atomic<std::uint64_t> consecutive_duplicate_frames{0};
    std::atomic<std::uint64_t> max_consecutive_duplicate_frames{0};

    std::atomic<float> audio_peak{0.0F};
    std::atomic<bool> video_write_failed{false};
    std::atomic<bool> audio_write_failed{false};
};

struct OutputFiles
{
    std::ofstream video;
    std::ofstream audio;
    std::mutex video_mutex;
    std::mutex audio_mutex;
    std::mutex output_mutex;
};

struct CameraTestResult
{
    std::shared_ptr<CAPTURE::ISource> source;
    bool source_created = false;
    bool property_ready = false;
    bool device_configured = false;
    bool format_configured = false;
    bool resolution_configured = false;
    int source_width = 0;
    int source_height = 0;
    std::string device_id;

    [[nodiscard]]
    bool IsConfigured() const noexcept
    {
        return source_created && property_ready && device_configured &&
               format_configured && resolution_configured;
    }
};

struct MicTestResult
{
    std::shared_ptr<CAPTURE::ISource> source;
    bool source_created = false;
    bool property_ready = false;
    bool device_configured = false;
    std::string device_id;

    [[nodiscard]]
    bool IsConfigured() const noexcept
    {
        return source_created && property_ready && device_configured;
    }
};

void UpdateMaximum(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value)
{
    std::uint64_t current = target.load(std::memory_order_relaxed);

    while (value > current &&
           !target.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed))
    {
    }
}

void UpdateMaximum(
    std::atomic<float>& target,
    float value)
{
    float current = target.load(std::memory_order_relaxed);

    while (value > current &&
           !target.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed))
    {
    }
}

/*
 * 对Y平面进行抽样哈希，不遍历每一个像素，减少采集回调中的计算量。
 * 连续数秒哈希完全相同，通常表示OBS正在重复输出最后一帧。
 */
std::uint64_t CalculateVideoHash(
    const CAPTURE::I420Frame& frame)
{
    const std::size_t data_size = frame.GetYPlaneSize();

    if (!frame.data[0] || data_size == 0)
    {
        return 0;
    }

    constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    constexpr std::size_t kMaximumSamples = 2048;

    const std::size_t step =
        std::max<std::size_t>(1, data_size / kMaximumSamples);

    std::uint64_t hash = kOffsetBasis;

    for (std::size_t index = 0; index < data_size; index += step)
    {
        hash ^= frame.data[0][index];
        hash *= kPrime;
    }

    return hash;
}

void RecordVideoContinuity(
    const CAPTURE::I420Frame& frame,
    CaptureStatistics& statistics)
{
    const std::uint64_t hash = CalculateVideoHash(frame);
    const std::uint64_t previous_hash =
        statistics.last_video_hash.exchange(
            hash,
            std::memory_order_relaxed);

    if (previous_hash != 0 && previous_hash == hash)
    {
        const std::uint64_t duplicate_count =
            statistics.consecutive_duplicate_frames.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

        UpdateMaximum(
            statistics.max_consecutive_duplicate_frames,
            duplicate_count);
    }
    else
    {
        statistics.consecutive_duplicate_frames.store(
            0,
            std::memory_order_relaxed);
    }
}

void RecordTimestamp(
    std::atomic<std::int64_t>& last_timestamp,
    std::atomic<std::uint64_t>& regressions,
    std::int64_t timestamp_us)
{
    const std::int64_t previous =
        last_timestamp.exchange(
            timestamp_us,
            std::memory_order_relaxed);

    if (previous >= 0 && timestamp_us < previous)
    {
        regressions.fetch_add(1, std::memory_order_relaxed);
    }
}

std::unique_ptr<CAPTURE::ICaptureEngine>
CreateInitializedCaptureEngine()
{
    auto engine = CAPTURE::ICaptureEngine::Create();

    if (!engine)
    {
        std::cerr << "创建CaptureEngine失败" << std::endl;
        return nullptr;
    }

    if (!engine->Init(
            kSampleRate,
            kChannels,
            kOutputWidth,
            kOutputHeight,
            kFps))
    {
        std::cerr << "初始化CaptureEngine失败" << std::endl;
        return nullptr;
    }

    std::cout
        << "CaptureEngine初始化成功\n"
        << "视频输出: " << kOutputWidth << "x" << kOutputHeight
        << " @ " << kFps << " FPS\n"
        << "音频输出: " << kSampleRate << " Hz, "
        << kChannels << " channels, Float32交错PCM"
        << std::endl;

    return engine;
}

bool OpenOutputFiles(OutputFiles& files)
{
    files.video.open(
        kVideoFileName,
        std::ios::binary | std::ios::trunc);

    if (!files.video.is_open())
    {
        std::cerr
            << "无法创建视频文件: "
            << kVideoFileName
            << std::endl;
        return false;
    }

    files.audio.open(
        kAudioFileName,
        std::ios::binary | std::ios::trunc);

    if (!files.audio.is_open())
    {
        std::cerr
            << "无法创建音频文件: "
            << kAudioFileName
            << std::endl;

        files.video.close();
        return false;
    }

    return true;
}

void CloseOutputFiles(OutputFiles& files)
{
    if (files.video.is_open())
    {
        files.video.flush();
        files.video.close();
    }

    if (files.audio.is_open())
    {
        files.audio.flush();
        files.audio.close();
    }
}

CameraTestResult ConfigureCamera(
    CAPTURE::ICaptureEngine& engine)
{
    CameraTestResult result;

    result.source = engine.CreateSource(
        CAPTURE::CaptureSourceType::kCST_Camera,
        kCameraSourceName);

    if (!result.source)
    {
        std::cerr << "创建摄像头Source失败" << std::endl;
        return result;
    }

    result.source_created = true;

    auto camera_source =
        std::dynamic_pointer_cast<CAPTURE::ICameraSource>(
            result.source);

    if (!camera_source)
    {
        std::cerr << "转换为ICameraSource失败" << std::endl;
        return result;
    }

    auto property = camera_source->GetProperty();

    if (!property)
    {
        std::cerr << "获取摄像头属性失败" << std::endl;
        return result;
    }

    result.property_ready = true;

    const auto devices = property->EnumCameraDevices();

    std::cout
        << "\n========== 摄像头设备 ==========\n"
        << "发现设备数量: " << devices.size()
        << std::endl;

    for (const auto& device : devices)
    {
        std::cout
            << "设备名称: " << device.name
            << "\n设备ID: " << device.id
            << std::endl;
    }

    if (devices.empty())
    {
        std::cerr << "未发现摄像头设备" << std::endl;
        return result;
    }

    auto selected_device = devices.begin();

    if (*kTargetCameraDeviceId != '\0')
    {
        selected_device = std::find_if(
            devices.begin(),
            devices.end(),
            [](const CAPTURE::CameraDeviceInfo& device)
            {
                return device.id == kTargetCameraDeviceId;
            });

        if (selected_device == devices.end())
        {
            std::cerr
                << "没有找到指定摄像头: "
                << kTargetCameraDeviceId
                << std::endl;
            return result;
        }
    }

    if (!property->SetVideoDevice(selected_device->id))
    {
        std::cerr
            << "设置摄像头设备失败: "
            << selected_device->id
            << std::endl;
        return result;
    }

    const auto current_device = property->GetCurrentDevice();

    if (!current_device ||
        current_device->id != selected_device->id)
    {
        std::cerr << "摄像头设备设置后校验失败" << std::endl;
        return result;
    }

    result.device_configured = true;
    result.device_id = selected_device->id;

    std::cout
        << "已选择摄像头: " << current_device->name
        << "\n设备ID: " << current_device->id
        << std::endl;

    /*
     * V4L2格式列表依赖当前设备，必须先设置设备再枚举格式。
     */
    const auto formats = property->EnumVideoFormats();

    std::cout
        << "\n========== 摄像头输入格式 ==========\n"
        << "支持格式数量: " << formats.size()
        << std::endl;

    for (const auto& format : formats)
    {
        std::cout
            << "格式名称: " << format.name
            << ", 格式值: " << format.value
            << std::endl;
    }

    const auto selected_format = std::find_if(
        formats.begin(),
        formats.end(),
        [](const CAPTURE::CameraVideoFormat& format)
        {
            return format.name.find(kTargetVideoFormat) !=
                   std::string::npos;
        });

    if (selected_format == formats.end())
    {
        std::cerr
            << "摄像头不支持目标格式: "
            << kTargetVideoFormat
            << std::endl;
        return result;
    }

    if (!property->SetVideoFormat(selected_format->value))
    {
        std::cerr
            << "设置摄像头格式失败: "
            << selected_format->name
            << std::endl;
        return result;
    }

    const auto current_format =
        property->GetCurrentVideoFormat();

    if (!current_format ||
        current_format->value != selected_format->value)
    {
        std::cerr << "摄像头格式设置后校验失败" << std::endl;
        return result;
    }

    result.format_configured = true;

    std::cout
        << "已选择摄像头格式: "
        << current_format->name
        << std::endl;

    /*
     * 分辨率列表依赖当前输入格式，必须在设置YUYV后重新枚举。
     */
    const auto resolutions = property->EnumResolutions();

    std::cout
        << "\n========== 摄像头输入分辨率 ==========\n"
        << "支持分辨率数量: " << resolutions.size()
        << std::endl;

    for (const auto& resolution : resolutions)
    {
        std::cout
            << resolution.width
            << "x"
            << resolution.height
            << std::endl;
    }

    const auto selected_resolution = std::find_if(
        resolutions.begin(),
        resolutions.end(),
        [](const CAPTURE::CameraResolution& resolution)
        {
            return resolution.width ==
                       static_cast<std::uint32_t>(kOutputWidth) &&
                   resolution.height ==
                       static_cast<std::uint32_t>(kOutputHeight);
        });

    if (selected_resolution == resolutions.end())
    {
        std::cerr
            << "当前摄像头格式不支持目标分辨率: "
            << kOutputWidth << "x" << kOutputHeight
            << std::endl;
        return result;
    }

    if (!property->SetResolution(*selected_resolution))
    {
        std::cerr << "设置摄像头分辨率失败" << std::endl;
        return result;
    }

    const auto current_resolution =
        property->GetCurrentResolution();

    if (!current_resolution ||
        current_resolution->width != selected_resolution->width ||
        current_resolution->height != selected_resolution->height)
    {
        std::cerr << "摄像头分辨率设置后校验失败" << std::endl;
        return result;
    }

    result.resolution_configured = true;

    std::cout
        << "已选择摄像头分辨率: "
        << current_resolution->width
        << "x"
        << current_resolution->height
        << std::endl;

    return result;
}

MicTestResult ConfigureMicrophone(
    CAPTURE::ICaptureEngine& engine)
{
    MicTestResult result;

    result.source = engine.CreateSource(
        CAPTURE::CaptureSourceType::kCST_Mic,
        kMicSourceName);

    if (!result.source)
    {
        std::cerr << "创建麦克风Source失败" << std::endl;
        return result;
    }

    result.source_created = true;

    auto mic_source =
        std::dynamic_pointer_cast<CAPTURE::IMicSource>(
            result.source);

    if (!mic_source)
    {
        std::cerr << "转换为IMicSource失败" << std::endl;
        return result;
    }

    auto property = mic_source->GetProperty();

    if (!property)
    {
        std::cerr << "获取麦克风属性失败" << std::endl;
        return result;
    }

    result.property_ready = true;

    const auto devices = property->EnumMicDevices();

    std::cout
        << "\n========== 麦克风设备 ==========\n"
        << "发现设备数量: " << devices.size()
        << std::endl;

    for (const auto& device : devices)
    {
        std::cout
            << "设备名称: " << device.name
            << "\n设备ID: " << device.id
            << std::endl;
    }

    if (devices.empty())
    {
        std::cerr << "未发现麦克风设备" << std::endl;
        return result;
    }

    const auto selected_device = std::find_if(
        devices.begin(),
        devices.end(),
        [](const CAPTURE::MicDeviceInfo& device)
        {
            return device.id == kTargetMicDeviceId;
        });

    if (selected_device == devices.end())
    {
        std::cerr
            << "没有找到指定麦克风: "
            << kTargetMicDeviceId
            << std::endl;
        return result;
    }

    if (!property->SetMicDevice(selected_device->id))
    {
        std::cerr
            << "设置麦克风失败: "
            << selected_device->name
            << std::endl;
        return result;
    }

    const auto current_device =
        property->GetCurrentMicDevice();

    if (!current_device ||
        current_device->id != selected_device->id)
    {
        std::cerr << "麦克风设备设置后校验失败" << std::endl;
        return result;
    }

    result.device_configured = true;
    result.device_id = selected_device->id;

    std::cout
        << "已选择麦克风: " << current_device->name
        << "\n设备ID: " << current_device->id
        << std::endl;

    return result;
}

void RegisterCaptureCallbacks(
    CAPTURE::ICaptureEngine& engine,
    OutputFiles& files,
    CaptureStatistics& statistics,
    bool save_video,
    bool save_audio)
{
    engine.RegisterVideoCallback(
        [&files, &statistics, save_video](
            const std::shared_ptr<CAPTURE::I420Frame>& frame)
        {
            if (!frame || !frame->IsValid())
            {
                statistics.invalid_video_frames.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return;
            }

            const std::uint64_t frame_number =
                statistics.video_frames.fetch_add(
                    1,
                    std::memory_order_relaxed) + 1;

            RecordTimestamp(
                statistics.last_video_timestamp_us,
                statistics.video_timestamp_regressions,
                frame->timestamp_us);

            if (frame->width != kOutputWidth ||
                frame->height != kOutputHeight)
            {
                statistics.video_format_errors.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return;
            }

            RecordVideoContinuity(*frame, statistics);

            if (save_video)
            {
                const std::size_t y_size =
                    frame->GetYPlaneSize();

                const std::size_t uv_size =
                    frame->GetUVPlaneSize();

                std::lock_guard<std::mutex> lock(
                    files.video_mutex);

                if (!statistics.video_write_failed.load(
                        std::memory_order_relaxed))
                {
                    files.video.write(
                        reinterpret_cast<const char*>(
                            frame->data[0].get()),
                        static_cast<std::streamsize>(y_size));

                    files.video.write(
                        reinterpret_cast<const char*>(
                            frame->data[1].get()),
                        static_cast<std::streamsize>(uv_size));

                    files.video.write(
                        reinterpret_cast<const char*>(
                            frame->data[2].get()),
                        static_cast<std::streamsize>(uv_size));

                    if (!files.video)
                    {
                        statistics.video_write_failed.store(
                            true,
                            std::memory_order_relaxed);
                    }
                    else
                    {
                        statistics.saved_video_frames.fetch_add(
                            1,
                            std::memory_order_relaxed);

                        statistics.video_bytes.fetch_add(
                            frame->GetDataSize(),
                            std::memory_order_relaxed);
                    }
                }
            }

            if (frame_number == 1)
            {
                std::lock_guard<std::mutex> lock(
                    files.output_mutex);

                std::cout
                    << "\n收到第一帧视频: "
                    << frame->width << "x" << frame->height
                    << ", timestamp=" << frame->timestamp_us
                    << " us, bytes=" << frame->GetDataSize()
                    << std::endl;
            }
        });

    engine.RegisterAudioCallback(
        [&files, &statistics, save_audio](
            const std::shared_ptr<CAPTURE::AudioFrame>& frame)
        {
            if (!frame || !frame->IsValid())
            {
                statistics.invalid_audio_frames.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return;
            }

            const std::uint64_t frame_number =
                statistics.audio_frames.fetch_add(
                    1,
                    std::memory_order_relaxed) + 1;

            RecordTimestamp(
                statistics.last_audio_timestamp_us,
                statistics.audio_timestamp_regressions,
                frame->timestamp_us);

            /*
             * CaptureEngine请求的是48kHz、双声道、Float32交错PCM。
             * 不符合该格式的数据不能写入当前测试文件。
             */
            if (frame->sample_rate != kSampleRate ||
                frame->channels != kChannels ||
                frame->samples <= 0)
            {
                statistics.audio_format_errors.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return;
            }

            const std::size_t sample_count =
                static_cast<std::size_t>(frame->samples) *
                static_cast<std::size_t>(frame->channels);

            const auto* samples =
                reinterpret_cast<const float*>(
                    frame->data.get());

            float frame_peak = 0.0F;

            for (std::size_t index = 0;
                 index < sample_count;
                 ++index)
            {
                if (std::isfinite(samples[index]))
                {
                    frame_peak = std::max(
                        frame_peak,
                        std::abs(samples[index]));
                }
            }

            UpdateMaximum(statistics.audio_peak, frame_peak);

            statistics.audio_samples.fetch_add(
                static_cast<std::uint64_t>(frame->samples),
                std::memory_order_relaxed);

            const std::size_t data_size =
                frame->GetDataSize();

            if (save_audio)
            {
                std::lock_guard<std::mutex> lock(
                    files.audio_mutex);

                if (!statistics.audio_write_failed.load(
                        std::memory_order_relaxed))
                {
                    files.audio.write(
                        reinterpret_cast<const char*>(
                            frame->data.get()),
                        static_cast<std::streamsize>(
                            data_size));

                    if (!files.audio)
                    {
                        statistics.audio_write_failed.store(
                            true,
                            std::memory_order_relaxed);
                    }
                    else
                    {
                        statistics.audio_bytes.fetch_add(
                            data_size,
                            std::memory_order_relaxed);
                    }
                }
            }

            if (frame_number == 1)
            {
                std::lock_guard<std::mutex> lock(
                    files.output_mutex);

                std::cout
                    << "\n收到第一帧音频: "
                    << frame->samples << " samples/channel, "
                    << frame->channels << " channels, "
                    << frame->sample_rate << " Hz, timestamp="
                    << frame->timestamp_us << " us"
                    << std::endl;
            }
        });
}

void UnregisterCaptureCallbacks(
    CAPTURE::ICaptureEngine& engine)
{
    engine.RegisterVideoCallback({});
    engine.RegisterAudioCallback({});
}

void UpdateCameraRuntimeState(
    CameraTestResult& camera)
{
    if (!camera.source)
    {
        return;
    }

    camera.source_width =
        camera.source->GetSourceWidth();

    camera.source_height =
        camera.source->GetSourceHeight();
}

bool RemoveAndReleaseSources(
    CAPTURE::ICaptureEngine& engine,
    CameraTestResult& camera,
    MicTestResult& mic)
{
    bool success = true;

    if (camera.source)
    {
        if (!engine.RemoveSource(kCameraSourceName))
        {
            std::cerr << "移除摄像头Source失败" << std::endl;
            success = false;
        }

        camera.source.reset();
    }

    if (mic.source)
    {
        if (!engine.RemoveSource(kMicSourceName))
        {
            std::cerr << "移除麦克风Source失败" << std::endl;
            success = false;
        }

        mic.source.reset();
    }

    return success;
}

void PrintTestResult(
    const CaptureStatistics& statistics,
    const CameraTestResult& camera,
    const MicTestResult& mic,
    bool source_cleanup_success,
    bool uninit_success)
{
    const std::uint64_t video_frames =
        statistics.video_frames.load(
            std::memory_order_relaxed);

    const std::uint64_t saved_video_frames =
        statistics.saved_video_frames.load(
            std::memory_order_relaxed);

    const std::uint64_t audio_frames =
        statistics.audio_frames.load(
            std::memory_order_relaxed);

    const std::uint64_t audio_samples =
        statistics.audio_samples.load(
            std::memory_order_relaxed);

    const double video_duration =
        static_cast<double>(saved_video_frames) /
        static_cast<double>(kFps);

    const double audio_duration =
        static_cast<double>(audio_samples) /
        static_cast<double>(kSampleRate);

    std::cout
        << "\n========== Capture测试结果 ==========\n"
        << "摄像头配置: "
        << (camera.IsConfigured() ? "成功" : "失败") << '\n'
        << "麦克风配置: "
        << (mic.IsConfigured() ? "成功" : "失败") << '\n'
        << "摄像头Source尺寸: "
        << camera.source_width << "x" << camera.source_height << '\n'
        << "视频回调帧数: " << video_frames << '\n'
        << "视频保存帧数: " << saved_video_frames << '\n'
        << "视频文件时长: " << video_duration << " 秒\n"
        << "视频文件字节数: "
        << statistics.video_bytes.load(
               std::memory_order_relaxed) << '\n'
        << "视频最大连续重复帧: "
        << statistics.max_consecutive_duplicate_frames.load(
               std::memory_order_relaxed) << '\n'
        << "视频时间戳回退: "
        << statistics.video_timestamp_regressions.load(
               std::memory_order_relaxed) << '\n'
        << "音频回调帧数: " << audio_frames << '\n'
        << "音频每声道采样数: " << audio_samples << '\n'
        << "音频文件时长: " << audio_duration << " 秒\n"
        << "音频文件字节数: "
        << statistics.audio_bytes.load(
               std::memory_order_relaxed) << '\n'
        << "音频峰值: "
        << statistics.audio_peak.load(
               std::memory_order_relaxed) << '\n'
        << "音频时间戳回退: "
        << statistics.audio_timestamp_regressions.load(
               std::memory_order_relaxed) << '\n'
        << "Source清理: "
        << (source_cleanup_success ? "成功" : "失败") << '\n'
        << "CaptureEngine反初始化: "
        << (uninit_success ? "成功" : "失败") << '\n'
        << "视频文件: " << kVideoFileName << '\n'
        << "音频文件: " << kAudioFileName
        << std::endl;
}

bool EvaluateTestResult(
    const CaptureStatistics& statistics,
    const CameraTestResult& camera,
    const MicTestResult& mic,
    bool source_cleanup_success,
    bool uninit_success)
{
    bool success =
        camera.IsConfigured() &&
        mic.IsConfigured() &&
        source_cleanup_success &&
        uninit_success;

    const std::uint64_t minimum_video_frames =
        static_cast<std::uint64_t>(
            kCaptureDuration.count() * kFps * 8 / 10);

    const std::uint64_t minimum_audio_samples =
        static_cast<std::uint64_t>(
            kCaptureDuration.count()) *
        static_cast<std::uint64_t>(kSampleRate) * 8 / 10;

    if (camera.source_width <= 0 ||
        camera.source_height <= 0)
    {
        std::cerr << "摄像头Source没有有效尺寸" << std::endl;
        success = false;
    }

    if (statistics.saved_video_frames.load(
            std::memory_order_relaxed) <
        minimum_video_frames)
    {
        std::cerr << "视频采集帧数不足" << std::endl;
        success = false;
    }

    /*
     * 连续3秒完全相同的抽样帧视为疑似摄像头停止。
     * 普通摄像头即使画面静止，传感器噪声通常也会使帧内容略有变化。
     */
    if (statistics.max_consecutive_duplicate_frames.load(
            std::memory_order_relaxed) >=
        static_cast<std::uint64_t>(kFps * 3))
    {
        std::cerr
            << "视频出现超过3秒的连续重复帧，摄像头可能已经停止"
            << std::endl;
        success = false;
    }

    if (statistics.audio_samples.load(
            std::memory_order_relaxed) <
        minimum_audio_samples)
    {
        std::cerr << "音频采样数量不足" << std::endl;
        success = false;
    }

    if (statistics.audio_peak.load(
            std::memory_order_relaxed) <= 0.00001F)
    {
        std::cerr
            << "音频数据接近全静音，请检查麦克风设备"
            << std::endl;
        success = false;
    }

    if (statistics.invalid_video_frames.load(
            std::memory_order_relaxed) != 0 ||
        statistics.video_format_errors.load(
            std::memory_order_relaxed) != 0)
    {
        std::cerr << "收到无效或格式错误的视频帧" << std::endl;
        success = false;
    }

    if (statistics.invalid_audio_frames.load(
            std::memory_order_relaxed) != 0 ||
        statistics.audio_format_errors.load(
            std::memory_order_relaxed) != 0)
    {
        std::cerr << "收到无效或格式错误的音频帧" << std::endl;
        success = false;
    }

    if (statistics.video_timestamp_regressions.load(
            std::memory_order_relaxed) != 0 ||
        statistics.audio_timestamp_regressions.load(
            std::memory_order_relaxed) != 0)
    {
        std::cerr << "采集时间戳出现回退" << std::endl;
        success = false;
    }

    if (statistics.video_write_failed.load(
            std::memory_order_relaxed) ||
        statistics.audio_write_failed.load(
            std::memory_order_relaxed))
    {
        std::cerr
            << "写入测试文件失败，请检查磁盘空间"
            << std::endl;
        success = false;
    }

    return success;
}

} // namespace

int main()
{
    std::cout
        << "========== Capture独立测试 ==========\n"
        << "测试时长: " << kCaptureDuration.count() << " 秒\n"
        << "目标视频格式: " << kTargetVideoFormat << '\n'
        << "目标视频尺寸: "
        << kOutputWidth << "x" << kOutputHeight << '\n'
        << "目标视频帧率: " << kFps << " FPS\n"
        << "目标音频格式: Float32交错PCM, "
        << kSampleRate << " Hz, "
        << kChannels << " channels"
        << std::endl;

    auto capture_engine =
        CreateInitializedCaptureEngine();

    if (!capture_engine)
    {
        return 1;
    }

    CameraTestResult camera =
        ConfigureCamera(*capture_engine);

    MicTestResult mic =
        ConfigureMicrophone(*capture_engine);

    OutputFiles output_files;

    if (!OpenOutputFiles(output_files))
    {
        RemoveAndReleaseSources(
            *capture_engine,
            camera,
            mic);

        capture_engine->UnInit();
        return 1;
    }

    CaptureStatistics statistics;

    RegisterCaptureCallbacks(
        *capture_engine,
        output_files,
        statistics,
        camera.IsConfigured(),
        mic.IsConfigured());

    std::cout
        << "\n开始采集，持续 "
        << kCaptureDuration.count()
        << " 秒..."
        << std::endl;

    std::this_thread::sleep_for(kCaptureDuration);

    /*
     * 必须先注销OBS回调，再关闭文件和释放回调引用的数据。
     */
    UnregisterCaptureCallbacks(*capture_engine);

    UpdateCameraRuntimeState(camera);
    CloseOutputFiles(output_files);

    const bool source_cleanup_success =
        RemoveAndReleaseSources(
            *capture_engine,
            camera,
            mic);

    const bool uninit_success =
        capture_engine->UnInit();

    PrintTestResult(
        statistics,
        camera,
        mic,
        source_cleanup_success,
        uninit_success);

    const bool success =
        EvaluateTestResult(
            statistics,
            camera,
            mic,
            source_cleanup_success,
            uninit_success);

    std::cout
        << "最终结果: "
        << (success ? "通过" : "失败")
        << std::endl;

    return success ? 0 : 2;
}

/*
 * 播放视频：
 *
  ffplay -autoexit \
  -f rawvideo \
  -pixel_format yuv420p \
  -video_size 640x480 \
  -framerate 30 \
  /home/ewas/VC_code/meeting-client/source/camera_640x480_i420.yuv
 *
 * 播放音频：
 *
  ffplay -autoexit -nodisp \
  -f f32le \
  -ar 48000 \
  -ac 2 \
  /home/ewas/VC_code/meeting-client/source/microphone_48000hz_2ch_f32le.pcm
 *
 * 注意：
 * 文件是双声道Float32交错PCM，不能使用“-ac 1”播放。
 * 使用“-ac 1”会把左右声道样本串行解释，声音会变慢、低沉。
 */