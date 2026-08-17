#pragma once

#include "IAudioSource.h"
#include "RenderDefine.h"

#include <miniaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RENDER {

/*
系统音频播放组件:
    AudioRender使用miniaudio打开Linux播放设备，并通过音频回调
    从IAudioSource读取交错Float32 PCM
*/
class AudioRender final {
public:
    AudioRender();
    ~AudioRender();

    AudioRender(const AudioRender&) = delete;
    AudioRender& operator=(const AudioRender&) = delete;
    AudioRender(AudioRender&&) = delete;
    AudioRender& operator=(AudioRender&&) = delete;

    // 设置播放数据源
    void SetAudioSource(std::shared_ptr<IAudioSource> source) noexcept;
        
    // 初始化并启动默认音频播放设备
    bool Initialize(int sample_rate, int channels);
        
    void Uninitialize();

    // 枚举系统当前可用的扬声器
    bool EnumSpeakers(std::vector<AudioSpeaker>& devices);
    
    bool SetSpeaker(const std::string& device_id);
        
    std::string GetCurrentSpeakerId() const;

private:
    // 工作线程支持的控制命令
    enum class AudioCommand : std::uint8_t {
        kNone = 0,
        kInitialize,
        kStart,
        kStop,
        kSwitchDevice,
        kEnumerateSpeakers,
        kShutdown
    };

    // 工作线程共享状态
    struct AudioWorkerState {
        std::mutex mutex;

        /*
         * command_cv用于通知工作线程有新命令, 防止控制线程写命令时，工作线程同时读取
            控制线程提交命令后，通过它唤醒工作线程

         * completion_cv用于通知控制线程命令已经完成, 工作线程执行完命令后，
         通过它唤醒正在SendCommandAndWait()中等待的控制线程
         */
        std::condition_variable command_cv;
        std::condition_variable completion_cv;

        AudioCommand command{AudioCommand::kNone};
        std::string target_device_id;

        bool command_pending{false}; // 表示当前命令槽里有一条尚未被工作线程取走的命令
        bool command_completed{false}; // 当前命令是否执行完毕
        bool command_success{false}; // 保存命令执行结果

        bool context_initialized{false}; // 表示ma_context是否已经初始化
        bool device_initialized{false}; // 表示ma_device是否已经初始化

        // 枚举扬声器命令的返回结果
        std::vector<AudioSpeaker> speaker_result;

        // 分别是miniaudio的运行上下文和播放设备对象，只允许工作线程创建、操作和释放
        ma_context context{};
        ma_device device{};
    };

private:
    // 提交命令并等待工作线程执行完成
    bool SendCommandAndWait(
        AudioCommand command,
        const std::string& device_id = {},
        std::vector<AudioSpeaker>* speakers = nullptr);

    // 音频设备工作线程入口
    static void WorkerThreadFunc(AudioRender* self) noexcept;

    //  miniaudio实时播放回调入口
    static void DataCallback(ma_device* device, void* output,
        const void* input, ma_uint32 frame_count) noexcept;
        
    // 从AudioMixer读取音频并写入播放缓冲区, 数据不足时写入静音
    void ProcessAudioCallback(void* output, ma_uint32 frame_count) noexcept;

    // 在工作线程中创建并启动播放设备
    bool InitDeviceInternal(const std::string& device_id);
        
    // 在工作线程中枚举播放设备
    bool EnumSpeakersInternal(std::vector<AudioSpeaker>& speakers);
        
private:
    AudioWorkerState m_worker_state;

    // 串行化Initialize和Uninitialize
    std::mutex m_lifecycle_mutex;

    // 串行化控制命令，避免命令被并发覆盖
    std::mutex m_command_submit_mutex;

    std::thread m_worker_thread;
    std::atomic<bool> m_worker_running{false};
    std::atomic<bool> m_initialized{false};

    /*
     * AudioMixer由RenderEngine和AudioRender共同持有。
     * 不存在反向引用，因此不会形成循环依赖。
     *
     * atomic<shared_ptr>避免实时回调与SetAudioSource并发时
     * 产生数据竞争。
     */
    std::shared_ptr<IAudioSource> m_audio_source;

    std::atomic<int> m_sample_rate{0};
    std::atomic<int> m_channels{0};

    /*
     * 当前设备ID由工作线程修改、控制线程读取，
     * 因此使用独立互斥锁保护。
     */
    mutable std::mutex m_device_mutex;
    std::string m_current_device_id;
};


} // namespace RENDER