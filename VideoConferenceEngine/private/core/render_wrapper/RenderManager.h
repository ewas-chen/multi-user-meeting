#pragma once

#include "VceTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RENDER {
class IRenderEngine;
} // namespace RENDER

namespace VCE {

/**
 * @brief 渲染模块包装器
 *
 * RenderManager负责管理RenderEngine生命周期，并将会议层的用户、
 * 音视频帧和扬声器控制请求转交给RenderEngine。
 *
 * 当前各模块使用相同的I420Frame和AudioFrame类型，因此音视频帧
 * 可以直接传递，不需要重新申请内存或复制数据。
 *
 * RenderEngine内部已经具有视频帧队列和音频混音缓冲，这里不再
 * 添加额外消费者线程，避免重复排队造成延迟。
 */
class RenderManager
{
public:
    RenderManager();
    ~RenderManager();

    RenderManager(const RenderManager&) = delete;
    RenderManager& operator=(const RenderManager&) = delete;
    RenderManager(RenderManager&&) = delete;
    RenderManager& operator=(RenderManager&&) = delete;

    // ==================== 生命周期 ====================

    Result Initialize(int sample_rate, int channels,
                      int video_width, int video_height);

    void Uninit();

    // ==================== 用户管理 ====================

    /**
     * @brief 为指定用户创建渲染资源
     *
     * 本地用户会启用镜像显示，远端用户正常显示。
     */
    Result AddUser(const std::string& user_name, bool is_local);

    /**
     * @brief 删除指定用户及其音视频渲染资源
     */
    Result RemoveUser(const std::string& user_name);

    /**
     * @brief 在当前OpenGL上下文中渲染指定用户的视频
     *
     * OpenGL上下文通常与创建它的线程绑定，因此该函数应由
     * UI或渲染线程调用，不能直接在RTC接收线程中调用。
     */
    Result RenderUserFrame(const std::string& user_name);

    /**
     * @brief 更新指定用户的视频显示区域大小
     */
    Result UpdateUserVideoSize(const std::string& user_name,
                               int width, int height);

    // ==================== 音视频输入 ====================

    /**
     * @brief 将I420视频帧放入指定用户的RenderEngine视频队列
     *
     * 该调用只传递shared_ptr，不复制Y、U、V平面数据。
     */
    Result PushVideoFrame(const std::string& user_name,
                          const std::shared_ptr<I420Frame>& frame);

    /**
     * @brief 将Float32 PCM音频帧交给RenderEngine混音
     *
     * 该调用只传递shared_ptr，不复制PCM数据。
     */
    Result PushAudioFrame(const std::string& user_name,
                          const std::shared_ptr<AudioFrame>& frame);

    // ==================== 扬声器管理 ====================

    Result GetAudioSpeakers(std::vector<SpeakerDeviceInfo>& speakers);

    Result UpdateAudioSpeaker(const std::string& speaker_id);

    Result GetCurrentAudioSpeaker(std::string& speaker_id);

private:
    /*
     * 保护RenderEngine生命周期。
     * Uninit与用户操作、帧输入不能同时访问m_render_engine。
     */
    mutable std::mutex m_state_mutex;

    std::unique_ptr<RENDER::IRenderEngine> m_render_engine;
    std::atomic<bool> m_initialized{false};
};

} // namespace VCE