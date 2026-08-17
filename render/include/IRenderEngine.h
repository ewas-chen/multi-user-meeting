#pragma once

#include "RenderDefine.h"

#include <memory>
#include <string>
#include <vector>

namespace RENDER {

// 音视频渲染引擎接口, 管理视频用户及其渲染资源
// RenderEngine只负责向当前OpenGL上下文绘制，不负责创建窗口、创建OpenGL上下文或交换前后缓冲区
class RENDER_ENGINE_API IRenderEngine {
public:
    virtual ~IRenderEngine() = default;

    IRenderEngine(const IRenderEngine&) = delete;
    IRenderEngine& operator=(const IRenderEngine&) = delete;
    IRenderEngine(IRenderEngine&&) = delete;
    IRenderEngine& operator=(IRenderEngine&&) = delete;

    static std::unique_ptr<IRenderEngine> CreateRenderEngine();

    virtual bool Initialize(int sample_rate, int channels, int video_width, int video_height) = 0;
        
    virtual void Uninitialize() = 0;
        
    // 添加一个待渲染用户,该函数会创建OpenGL纹理、Mesh等资源，调用时必须存在当前线程可用的OpenGL上下文
    virtual bool AddUser(const std::string& user_name, bool is_local) = 0;
        
    virtual bool RemoveUser(const std::string& user_name) = 0;
       
    // 将指定用户的最新可用视频帧绘制到当前OpenGL上下文
    virtual bool RenderUser(const std::string& user_name) = 0;
        
    // 更新指定用户的视频渲染区域大小
    virtual bool UpdateUserVideoSize(const std::string& user_name, int width, int height) = 0;

    // 向指定用户的视频缓冲区推送一帧I420数据
    virtual bool PushVideoFrame(const std::string& user_name, const std::shared_ptr<I420Frame>& frame) = 0;
    
    // 向音频混音器推送一帧Float32 PCM音频
    virtual bool PushAudioFrame(const std::string& user_name, const std::shared_ptr<AudioFrame>& frame) = 0;
        
    // 枚举当前系统可用的音频播放设备
    virtual std::vector<AudioSpeaker> GetSpeakerDevices() = 0;
    
    // 切换音频播放设备
    virtual bool UpdateAudioSpeaker(const std::string& device_id) = 0;
        
    // 获取当前音频播放设备ID
    virtual bool GetCurrentAudioSpeaker(std::string& device_id) = 0;

protected:
    IRenderEngine() = default;
};


} // namespace