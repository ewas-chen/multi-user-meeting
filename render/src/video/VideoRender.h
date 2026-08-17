#pragma once

#include "RenderDefine.h"

#include <memory>
#include <string>

namespace RENDER {

class VideoMesh;
class VideoShader;
class YUVTexture;

/**
 * @brief 负责单个用户的视频画面渲染
 *
 * VideoRender组合以下三个对象：
 *
 * VideoMesh：
 *   管理用于显示视频的矩形；
 *
 * YUVTexture：
 *   将I420Frame上传为Y、U、V三张OpenGL纹理；
 *
 * VideoShader：
 *   在GPU中把I420转换成RGB，并处理水平镜像。
 *
 * VideoRender不管理视频帧队列。
 * PushVideoFrame()传入的帧由UserContext缓存，
 * UserContext选出需要显示的帧后再调用RenderFrame()。
 *
 * 所有成员函数都必须在创建对应OpenGL资源的渲染线程，
 * 且持有正确OpenGL上下文时调用。
 */
class RENDER_ENGINE_LOCAL VideoRender final {
public:
    /**
     * @param user_id           当前渲染对象所属用户
     * @param shader            多个用户共用的I420 Shader
     * @param mirror_horizontal 是否水平镜像，通常本地用户为true
     */
    VideoRender(
        std::string user_id,
        std::shared_ptr<VideoShader> shader,
        bool mirror_horizontal);

    ~VideoRender() noexcept;

    VideoRender(const VideoRender&) = delete;
    VideoRender& operator=(const VideoRender&) = delete;
    VideoRender(VideoRender&&) = delete;
    VideoRender& operator=(VideoRender&&) = delete;

    /**
     * @brief 初始化矩形和I420纹理资源
     *
     * 调用前，传入的共享VideoShader必须已经初始化。
     *
     * @param video_width  初始视频宽度
     * @param video_height 初始视频高度
     */
    bool Init(
        int video_width,
        int video_height);

    /**
     * @brief 释放当前用户拥有的OpenGL资源
     *
     * 只释放VideoMesh和YUVTexture。
     * 共享VideoShader由RenderEngine统一管理。
     */
    void Uninitialize() noexcept;

    /**
     * @brief 上传并绘制一帧I420视频
     *
     * 主要流程：
     *
     * 1. 设置Viewport；
     * 2. 清理当前颜色缓冲区；
     * 3. 上传Y、U、V三个平面；
     * 4. 启用VideoShader；
     * 5. 绑定三张纹理；
     * 6. 绘制视频矩形。
     */
    bool RenderFrame(
        const std::shared_ptr<I420Frame>& frame);

    /**
     * @brief 将当前Viewport清理为黑色
     *
     * 该函数只清理OpenGL颜色缓冲区，
     * 不负责清理UserContext中的视频帧队列。
     */
    bool ClearBuffer() const noexcept;

    /**
     * @brief 修改当前用户画面的Viewport尺寸
     *
     * 这里修改的是最终显示区域尺寸，
     * 不是摄像头输入帧的分辨率。
     */
    bool ResizeVideo(
        int width,
        int height) noexcept;

    [[nodiscard]]
    const std::string& GetUserId() const noexcept {
        return m_user_id;
    }

    [[nodiscard]]
    bool IsInitialized() const noexcept {
        return m_initialized;
    }

private:
    std::string m_user_id;

    bool m_initialized{false};

    /*
     * 本地摄像头预览通常需要水平镜像，
     * 远端用户视频通常不镜像。
     */
    bool m_mirror_horizontal{false};

    /*
     * 最终视频显示区域的尺寸。
     * RenderFrame()绘制前会使用它们设置glViewport。
     */
    int m_viewport_width{0};
    int m_viewport_height{0};

    // 每个用户拥有独立的矩形资源
    std::unique_ptr<VideoMesh> m_mesh;

    // 每个用户拥有独立的Y、U、V纹理
    std::unique_ptr<YUVTexture> m_yuv_texture;

    /*
     * Shader程序由多个用户共享，减少重复编译和链接。
     *
     * 如果不同用户使用不同OpenGL Context，
     * 这些Context必须属于同一个OpenGL资源共享组；
     * 否则Shader Program不能跨Context直接使用。
     */
    std::shared_ptr<VideoShader> m_shader;
};

} // namespace RENDER