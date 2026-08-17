// VideoMesh 只管理用于显示视频的矩形以及对应的 OpenGL 顶点资源，不负责 Shader 和纹理
#pragma once

#include "RenderDefine.h"

namespace RENDER {

/**
 * 管理视频画面使用的矩形网格
 *
 * 视频最终会被绘制到一个由两个三角形组成的矩形上。
 *
 * VideoMesh只负责：
 * 1. 创建并保存矩形的顶点数据；
 * 2. 创建顶点索引；
 * 3. 配置顶点位置和纹理坐标；
 * 4. 发出OpenGL绘制指令。
 *
 * 它不负责Shader、I420纹理和视频帧数据。
 *
 * Init()、Uninit()和Draw()必须在持有有效OpenGL上下文的
 * 渲染线程中调用。
 */
class RENDER_ENGINE_LOCAL VideoMesh final {
public:
    VideoMesh() = default;
    ~VideoMesh() noexcept;

    VideoMesh(const VideoMesh&) = delete;
    VideoMesh& operator=(const VideoMesh&) = delete;
    VideoMesh(VideoMesh&&) = delete;
    VideoMesh& operator=(VideoMesh&&) = delete;

    /**
     * 创建矩形使用的VAO、VBO和EBO
     *
     * 顶点属性与Vertex Shader保持一致：
     * location 0：二维顶点位置；
     * location 1：二维纹理坐标。
     */
    bool Init();

    /**
     * 释放VAO、VBO和EBO
     *
     * 调用时必须保证OpenGL上下文仍然有效。
     */
    void Uninit() noexcept;

    /**
     * 绘制视频矩形
     *
     * 内部会绑定VAO、绘制两个三角形，然后解除VAO绑定。
     */
    void Draw() const noexcept;

    /**
     * 绑定当前矩形的VAO
     *
     * 一般由Draw()内部使用。当外部需要组合多个OpenGL操作时，
     * 也可以手动调用Bind()和Unbind()。
     */
    void Bind() const noexcept;

    /**
     * 解除当前VAO绑定
     */
    void Unbind() const noexcept;

    /**
     * 判断矩形的OpenGL资源是否创建完成
     */
    [[nodiscard]]
    bool IsInitialized() const noexcept {
        return m_initialized;
    }

private:
    bool m_initialized{false};

    // Vertex Array Object：记录顶点属性和缓冲区绑定关系
    unsigned int m_vao{0};

    // Vertex Buffer Object：保存矩形的顶点位置和纹理坐标
    unsigned int m_vbo{0};

    // Element Buffer Object：保存组成矩形的顶点索引
    unsigned int m_ebo{0};
};

} // namespace RENDER