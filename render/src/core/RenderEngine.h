#pragma once

#include "IRenderEngine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace RENDER {
class AudioMixer; // 接收并混合多个用户的音频
class AudioRender; // 将混音结果输出到扬声器
class VideoShader; // 所有用户共享的I420转RGB Shader
class UserContext; // 管理单个用户的视频帧队列和渲染资源

// 音视频渲染引擎实现
class RENDER_ENGINE_LOCAL RenderEngine final : public IRenderEngine {
public:
    RenderEngine();
    ~RenderEngine() override;

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&) = delete;
    RenderEngine& operator=(RenderEngine&&) = delete;

    bool Initialize(int sample_rate, int channels, int video_width, int video_height) override;
        
    void Uninitialize() override;
        
    bool AddUser(const std::string& user_name, bool is_local) override;
        
    bool RemoveUser(const std::string& user_name) override;
       
    bool RenderUser(const std::string& user_name) override;
        
    bool UpdateUserVideoSize(const std::string& user_name, int width, int height) override;

    bool PushVideoFrame(const std::string& user_name, const std::shared_ptr<I420Frame>& frame) override;
    
    bool PushAudioFrame(const std::string& user_name, const std::shared_ptr<AudioFrame>& frame) override;
        
    std::vector<AudioSpeaker> GetSpeakerDevices() override;
    
    bool UpdateAudioSpeaker(const std::string& device_id) override;
        
    bool GetCurrentAudioSpeaker(std::string& device_id) override;

private:
    // 查找用户上下文
    std::shared_ptr<UserContext>
    GetUserContext(const std::string& user_name) const;

private:

    // AudioRender通过weak_ptr<IAudioSource>读取AudioMixer数据
    std::shared_ptr<AudioMixer> m_audio_mixer;

    // AudioRender只由RenderEngine拥有，不需要共享所有权
    std::unique_ptr<AudioRender> m_audio_render;

    // 同一个OpenGL上下文中的所有用户共享一套Shader程序
    std::shared_ptr<VideoShader> m_video_shader;

    mutable std::mutex m_users_mutex;
    std::unordered_map<std::string, std::shared_ptr<UserContext>> m_user_contexts;
        
    // 串行初始化
    mutable std::mutex m_state_mutex;

    int m_video_width{0};
    int m_video_height{0};
    std::atomic<bool> m_initialized{false};
};


} // namespace RENDER