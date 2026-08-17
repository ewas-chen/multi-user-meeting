#pragma once

#include "CaptureDefine.h"
#include "ISource.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace CAPTURE {

/**
 * 紧凑存储的 I420 视频帧。
 *
 * 内存布局：
 *   data[0]：Y 平面，width * height 字节
 *   data[1]：U 平面，width/2 * height/2 字节
 *   data[2]：V 平面，width/2 * height/2 字节
 *
 * 每个像素分量占用一个字节，平面内部不包含 stride padding。
 */
struct I420Frame {
    std::shared_ptr<std::uint8_t[]> data[3]; // Y,U,V各自内存首地址

    int width  = 0;
    int height = 0;

    // 单调时钟时间戳，单位为微秒
    /*
    控制播放速度
    根据相邻帧的时间差，决定什么时候显示下一帧;

    音视频同步
    将视频帧时间戳与音频帧时间戳比较，判断视频应该等待、立即播放还是丢帧。

    判断帧顺序
    时间戳较小的帧通常先采集，便于处理网络乱序或异步处理造成的顺序变化。

    计算延迟和帧率
    当前时间减去帧时间戳，可以估算采集、编码、网络传输和解码延迟
    */
    std::int64_t timestamp_us = 0;

    I420Frame() = default;
    ~I420Frame() = default;

    I420Frame(const I420Frame&) = default;
    I420Frame& operator=(const I420Frame&) = default;

    I420Frame(I420Frame&&) noexcept = default;
    I420Frame& operator=(I420Frame&&) noexcept = default;

    [[nodiscard]]
    std::size_t GetYPlaneSize() const noexcept {
        if (width <= 0 || height <= 0) {
            return 0;
        }

        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    [[nodiscard]]
    std::size_t GetUVPlaneSize() const noexcept
    {
        if (width <= 0 || height <= 0) {
            return 0;
        }

        return static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2);
    }

    [[nodiscard]]
    std::size_t GetDataSize() const noexcept {
        return GetYPlaneSize() + 2 * GetUVPlaneSize();
    }

    [[nodiscard]]
    bool IsValid() const noexcept
    {
        return data[0] && data[1] && data[2] &&
               width > 0 && height > 0 && width % 2 == 0 && height % 2 == 0;
    }
};

/**
 * 交错存储的 Float32 PCM 音频帧。
 *
 * 数据格式：
 *
 *   单声道：[sample0][sample1]...
 *   双声道：[L0][R0][L1][R1]...
 *
 * data 实际存储 float 样本，但使用 uint8_t 指针保存原始数据，
 */
struct AudioFrame
{
    std::shared_ptr<std::uint8_t[]> data;

    // 每个声道包含的采样点数量
    int samples = 0; // 多少采样点

    int channels   = 0; // 多少声道
    int sample_rate = 0; // 每秒采多少次

    // 单调时钟时间戳，单位为微秒
    std::int64_t timestamp_us = 0;

    AudioFrame() = default;
    ~AudioFrame() = default;

    AudioFrame(const AudioFrame&) = default;
    AudioFrame& operator=(const AudioFrame&) = default;

    AudioFrame(AudioFrame&&) noexcept = default;
    AudioFrame& operator=(AudioFrame&&) noexcept = default;

    [[nodiscard]]
    std::size_t GetDataSize() const noexcept {
        if (samples <= 0 || channels <= 0) {
            return 0;
        }

        return static_cast<std::size_t>(samples) * static_cast<std::size_t>(channels) *
               sizeof(float);
    }

    [[nodiscard]]
    bool IsValid() const noexcept {
        return data && samples > 0 && channels > 0 && sample_rate > 0;
    }
};

/**
 * 视频帧回调
 *
 * OBS 采集线程先调用回调，回调（VideoDataCallback，AudioDataCallback）负责把帧送进队列，
 * 处理线程取出后调用的是编码器、发送器等后续处理函数
 * 
 * 回调运行在 OBS 视频采集线程中，调用者不应执行耗时或阻塞操作
 * 需要异步处理时，应将 shared_ptr 放入有界队列。
 */
using VideoDataCallback =
    std::function<void(const std::shared_ptr<I420Frame>& frame)>;

/**
 * 音频帧回调
 *
 * 回调运行在 OBS 音频采集线程中，调用者不应执行耗时或阻塞操作
 */
using AudioDataCallback =
    std::function<void(const std::shared_ptr<AudioFrame>& frame)>;

/**
 * 采集模块的核心公共接口
 *
 * 上层只通过该接口使用采集模块，不需要了解 OBS、V4L2 或 PulseAudio/PipeWire 的实现细节
 */
class CAPTURE_ENGINE_API ICaptureEngine {
public:
    virtual ~ICaptureEngine() = default;

    // 创建采集引擎实例。
    [[nodiscard]]
    static std::unique_ptr<ICaptureEngine> Create();

    /**
     * 初始化采集引擎：
     *  sample_rate 音频采样率，例如 48000。
     *  channels    音频声道数，当前支持 1 或 2。
     *  width       视频输出宽度，必须为正偶数。
     *  height      视频输出高度，必须为正偶数。
     *  fps         视频输出帧率(相邻视频帧的时间差越小帧率越大)。
     */
    [[nodiscard]]
    virtual bool Init(int sample_rate, int channels, int width, int height, int fps) = 0;

    /**
     * 反初始化采集引擎并释放所有 OBS 资源。
     *
     * 该函数应支持重复调用。
     */
    virtual bool UnInit() = 0;

    /**
     * 注册或替换视频帧回调。
     *
     * 传入空回调表示注销当前视频回调。
     */
    virtual void RegisterVideoCallback(VideoDataCallback callback) = 0;

    /**
     * 注册或替换音频帧回调。
     *
     * 传入空回调表示注销当前音频回调。
     */
    virtual void RegisterAudioCallback(AudioDataCallback callback) = 0;

    /**
     * 创建采集源。
     *
     * 同一个引擎内，采集源名称必须唯一。
     */
    [[nodiscard]]
    virtual std::shared_ptr<ISource> CreateSource(CaptureSourceType type, const std::string& name) = 0;
        
    // 根据名称删除采集源
    [[nodiscard]]
    virtual bool RemoveSource(const std::string& name) = 0;

    /**
     * 根据名称查询采集源。
     *
     * 未找到时返回 nullptr。
     */
    [[nodiscard]]
    virtual std::shared_ptr<ISource> GetSource(const std::string& name) = 0;

    // 获取当前引擎管理的全部采集源
    [[nodiscard]]
    virtual std::vector<std::shared_ptr<ISource>> GetAllSources() = 0;

    // 获取当前已加入活动 OBS 场景的采集源
    [[nodiscard]]
    virtual std::vector<std::shared_ptr<ISource>> GetActiveSources() = 0;

    // 查询采集引擎是否已经初始化
    [[nodiscard]]
    virtual bool IsInitialized() const noexcept = 0;
};

} // namespace CAPTURE