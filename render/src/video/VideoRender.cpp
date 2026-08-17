#include "VideoRender.h"

#include "GLFrameWork/VideoMesh.h"
#include "GLFrameWork/VideoShader.h"
#include "GLFrameWork/YUVTexture.h"
#include "utils/logManager.h"

#include <glad/glad.h>

#include <utility>

namespace RENDER {

VideoRender::VideoRender(
    std::string user_id,
    std::shared_ptr<VideoShader> shader,
    bool mirror_horizontal)
    : m_user_id(std::move(user_id)),
      m_mirror_horizontal(mirror_horizontal),
      m_mesh(std::make_unique<VideoMesh>()),
      m_yuv_texture(std::make_unique<YUVTexture>()),
      m_shader(std::move(shader)) {
}

VideoRender::~VideoRender() noexcept {
    Uninitialize();
}

bool VideoRender::Init(
    int video_width,
    int video_height) {

    if (m_user_id.empty()) {
        LOG_ERROR(
            "Failed to initialize VideoRender: "
            "user id is empty");

        return false;
    }

    if (video_width <= 0 ||
        video_height <= 0 ||
        (video_width % 2) != 0 ||
        (video_height % 2) != 0) {

        LOG_ERROR(
            "Failed to initialize VideoRender for {}: "
            "invalid video size {}x{}",
            m_user_id,
            video_width,
            video_height);

        return false;
    }

    if (!m_shader ||
        !m_shader->IsInitialized()) {

        LOG_ERROR(
            "Failed to initialize VideoRender for {}: "
            "video shader is unavailable",
            m_user_id);

        return false;
    }

    if (m_initialized) {
        /*
         * 已经初始化时只更新显示区域尺寸。
         * 后续视频帧尺寸发生变化时，YUVTexture会自动调整。
         */
        m_viewport_width = video_width;
        m_viewport_height = video_height;
        return true;
    }

    /*
     * VideoShader::Initialize()已经完成GLAD加载，
     * 因此这里可以创建OpenGL顶点资源。
     */
    if (!m_mesh->Init()) {
        LOG_ERROR(
            "Failed to initialize VideoMesh for user: {}",
            m_user_id);

        return false;
    }

    if (!m_yuv_texture->Initialize(
            video_width,
            video_height)) {

        LOG_ERROR(
            "Failed to initialize YUVTexture for user: {}",
            m_user_id);

        /*
         * YUVTexture初始化失败时回滚已经创建的Mesh资源，
         * 避免对象停留在只初始化一半的状态。
         */
        m_mesh->Uninit();
        return false;
    }

    m_viewport_width = video_width;
    m_viewport_height = video_height;
    m_initialized = true;

    /*
     * 默认使用黑色清理背景。
     * alpha为1.0，表示完全不透明。
     */
    glClearColor(
        0.0F,
        0.0F,
        0.0F,
        1.0F);

    LOG_INFO(
        "VideoRender initialized: "
        "user={}, size={}x{}, mirror={}",
        m_user_id,
        video_width,
        video_height,
        m_mirror_horizontal);

    return true;
}

void VideoRender::Uninitialize() noexcept {
    /*
     * 不能只判断m_initialized。
     * 如果Init()只完成了一部分，也需要允许释放已有资源。
     */
    if (m_yuv_texture) {
        m_yuv_texture->Uninit();
    }

    if (m_mesh) {
        m_mesh->Uninit();
    }

    m_viewport_width = 0;
    m_viewport_height = 0;
    m_initialized = false;

    /*
     * 不在这里释放或Uninit共享Shader。
     * Shader由RenderEngine统一管理，并可能仍被其他用户使用。
     *
     * 同时不reset m_shader，使VideoRender可以在Uninitialize()
     * 后重新调用Init()。
     */
}

bool VideoRender::RenderFrame(
    const std::shared_ptr<I420Frame>& frame) {

    if (!m_initialized) {
        LOG_ERROR(
            "Failed to render video for {}: "
            "VideoRender is not initialized",
            m_user_id);

        return false;
    }

    if (!frame || !frame->IsValid()) {
        LOG_ERROR(
            "Failed to render video for {}: "
            "frame is invalid",
            m_user_id);

        return false;
    }

    if (!m_shader ||
        !m_shader->IsInitialized()) {

        LOG_ERROR(
            "Failed to render video for {}: "
            "video shader is unavailable",
            m_user_id);

        return false;
    }

    /*
     * Viewport决定标准化坐标[-1, 1]最终映射到
     * 窗口中的哪块像素区域。
     */
    glViewport(
        0,
        0,
        m_viewport_width,
        m_viewport_height);

    /*
     * OpenGL状态可能被其他渲染代码改变，
     * 因此每次清屏前明确设置黑色背景。
     */
    glClearColor(
        0.0F,
        0.0F,
        0.0F,
        1.0F);

    glClear(GL_COLOR_BUFFER_BIT);

    /*
     * 将CPU内存中的三个I420平面上传到GPU纹理。
     * 视频分辨率发生变化时，YUVTexture内部会自动重建纹理。
     */
    if (!m_yuv_texture->UpdateYUVData(frame)) {
        LOG_ERROR(
            "Failed to update I420 textures for user: {}",
            m_user_id);

        return false;
    }

    /*
     * 激活共享的I420 Shader Program。
     */
    m_shader->Use();

    /*
     * Shader由多个用户共享，因此镜像状态必须在每次绘制前设置。
     *
     * 本地用户通常镜像，远端用户不镜像。
     */
    m_shader->SetMirrorHorizontal(
        m_mirror_horizontal);

    /*
     * 绑定关系必须与VideoShader中的sampler一致：
     *
     * Y -> 纹理单元0
     * U -> 纹理单元1
     * V -> 纹理单元2
     */
    m_yuv_texture->BindTextures(
        0,
        1,
        2);

    /*
     * VideoShader初始化时已经调用：
     *
     * SetYUVTextures(0, 1, 2)
     *
     * 纹理单元关系不会随帧变化，因此这里无需每帧重复设置。
     */
    m_mesh->Draw();

    /*
     * 清理本次绘制使用的OpenGL绑定状态。
     */
    m_yuv_texture->UnbindTextures(
        0,
        1,
        2);

    m_shader->Unuse();

    /*
     * 这里只提交OpenGL绘制指令。
     * 窗口交换缓冲区由上层UI负责，例如：
     *
     * glfwSwapBuffers(window);
     */
    return true;
}

bool VideoRender::ClearBuffer() const noexcept {
    if (!m_initialized ||
        m_viewport_width <= 0 ||
        m_viewport_height <= 0) {

        return false;
    }

    glViewport(
        0,
        0,
        m_viewport_width,
        m_viewport_height);

    glClearColor(
        0.0F,
        0.0F,
        0.0F,
        1.0F);

    glClear(GL_COLOR_BUFFER_BIT);

    return true;
}

bool VideoRender::ResizeVideo(
    int width,
    int height) noexcept {

    if (!m_initialized ||
        width <= 0 ||
        height <= 0) {

        return false;
    }

    /*
     * 只保存新的显示区域尺寸。
     * 下一次RenderFrame()或ClearBuffer()时再设置Viewport，
     * 避免依赖ResizeVideo()调用后OpenGL状态一直不被修改。
     */
    m_viewport_width = width;
    m_viewport_height = height;

    return true;
}

} // namespace RENDER