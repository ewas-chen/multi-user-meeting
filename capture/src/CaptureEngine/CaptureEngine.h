#pragma once

#include "ICaptureEngine.h"

#include <obs/obs.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace CAPTURE {

class CaptureEngine final : public ICaptureEngine {
public:
    CaptureEngine() = default;
    ~CaptureEngine() override;

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    CaptureEngine(CaptureEngine&&) = delete;
    CaptureEngine& operator=(CaptureEngine&&) = delete;

public:
    bool Init(int sample_rate, int channels, int width, int height, int fps) override;

    bool UnInit() override;

    void RegisterVideoCallback(VideoDataCallback callback) override;
        
    void RegisterAudioCallback(AudioDataCallback callback) override;

    std::shared_ptr<ISource> CreateSource(CaptureSourceType type, const std::string& name) override;
        

    bool RemoveSource(const std::string& name) override;

    std::shared_ptr<ISource> GetSource(const std::string& name) override;

    std::vector<std::shared_ptr<ISource>> GetAllSources() override;

    std::vector<std::shared_ptr<ISource>> GetActiveSources() override;

    bool IsInitialized() const noexcept override;

private:
    /**
     * OBS 原始视频帧回调。
     * 该函数由 OBS 视频线程调用, OBS 每生成一帧最终视频输出，就会自动调用
     * 读取 OBS 帧信息->复制帧数据->构造自己的 Frame 对象->快速交给队列或上层回调
     */
    static void OnRawVideoData(void* param, struct video_data* frame);

    /**
     * OBS 原始音频帧回调。
     * 该函数由 OBS 音频线程调用, OBS 音频线程每处理出一批混音后的 PCM 数据，就会调用
     */
    static void OnRawAudioData(void* param, std::size_t mix_index, struct audio_data* frame);

private:
    std::shared_ptr<ISource> CreateCameraSource(const std::string& name);

    std::shared_ptr<ISource> CreateMicSource(const std::string& name);

    std::shared_ptr<I420Frame> ConvertVideoData(const struct video_data* obs_frame) const;

    std::shared_ptr<AudioFrame> ConvertAudioData(const struct audio_data* obs_frame) const;
        
private:
    /**
     * 解除已经注册的 OBS 原始数据回调。
     *
     * 调用后仍会清空上层保存的 std::function。
     */
    void UnregisterRawCallbacks() noexcept;

    /**
     * 删除全部采集源。
     *
     * 调用该函数前必须持有 m_state_mutex。
     */
    void RemoveAllSources() noexcept;

    /**
     * 清理当前引擎持有的全部 OBS 状态。
     *
     * 用于正常反初始化以及 Init() 中途失败后的回滚。
     * 调用该函数前必须持有 m_state_mutex。
     */
    void CleanupObsState() noexcept;

private:
    /**
     * 保护初始化、反初始化、默认场景和采集源容器。
     *
     * 音视频帧回调中不能获取该锁，避免阻塞 OBS 实时线程。
     */
    mutable std::mutex m_state_mutex;

    /**
     * 只有完整初始化成功后才为 true。
     *
     * 使用 atomic 是为了让 IsInitialized() 和 OBS 回调能够
     * 在不获取 m_state_mutex 的情况下安全读取状态。
     */
    std::atomic_bool m_initialized{false};

    /**
     * 表示之前是否已经成功执行过 obs_startup。
     */
    bool m_obs_acquired{false};

    /**
     * CaptureEngine 默认场景(场景（Scene）就是一个“画面容器”或“画布”,就是多个Source的集合)
     * 刚创建时，场景是空的，后续添加Source
     * 摄像头和麦克风采集源都会被添加到该场景。
     */
    OBSScene m_default_scene;

    /**
     * 以采集源名称为键保存当前引擎管理的所有源。
     */
    std::unordered_map<std::string, std::shared_ptr<ISource>> m_sources;

private:
    /**
     * 只保护上层回调函数及 OBS 回调注册状态。
     *
     * 调用上层回调前必须先释放该锁。
     */
    mutable std::mutex m_callback_mutex;

    VideoDataCallback m_video_callback;
    AudioDataCallback m_audio_callback;

    /**
     * 防止重复调用 obs_add_raw_*_callback()。
     */
    bool m_video_callback_registered{false};
    bool m_audio_callback_registered{false};

private:
    /**
     * OBS 音频输出配置。
     *
     * RegisterAudioCallback() 配置音频格式以及转换音频帧时使用。
     */
    int m_sample_rate{48000};
    int m_channels{2};
};

} // namespace CAPTURE