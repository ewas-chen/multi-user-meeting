#include "VideoMesh.h"

#include "utils/logManager.h"

#include <glad/glad.h>

#include <array>
#include <cstddef>

namespace {

/**
 * 每个顶点包含：
 * 1. 二维屏幕位置：position_x、position_y；
 * 2. 二维纹理坐标：texture_x、texture_y。
 */
struct VideoVertex {
    float position_x;
    float position_y;
    float texture_x;
    float texture_y;
};

/*
 * OpenGL标准化设备坐标范围为[-1, 1]。
 * 下面四个顶点覆盖整个当前Viewport。
 *
 * 视频帧第一行对应纹理顶部，因此：
 * 左上角使用纹理坐标(0, 0)；
 * 右下角使用纹理坐标(1, 1)。
 */
constexpr std::array<VideoVertex, 4> kVideoVertices{{
    {-1.0F,  1.0F, 0.0F, 0.0F}, // 左上角
    {-1.0F, -1.0F, 0.0F, 1.0F}, // 左下角
    { 1.0F, -1.0F, 1.0F, 1.0F}, // 右下角
    { 1.0F,  1.0F, 1.0F, 0.0F}  // 右上角
}};

// 一个矩形由两个三角形组成
constexpr std::array<unsigned int, 6> kVideoIndices{{
    0, 1, 2,
    0, 2, 3
}};

} // namespace

namespace RENDER {

VideoMesh::~VideoMesh() noexcept {
    Uninit();
}

bool VideoMesh::Init() {
    if (m_initialized) {
        return true;
    }

    /*
     * VAO记录当前矩形的顶点属性配置，
     * 包括VBO的使用方式以及EBO的绑定关系。
     */
    glGenVertexArrays(1, &m_vao);

    /*
     * VBO保存顶点位置和纹理坐标。
     */
    glGenBuffers(1, &m_vbo);

    /*
     * EBO保存组成两个三角形的顶点索引。
     */
    glGenBuffers(1, &m_ebo);

    if (m_vao == 0 || m_vbo == 0 || m_ebo == 0) {
        LOG_ERROR(
            "Failed to create video mesh resources: "
            "vao={}, vbo={}, ebo={}",
            m_vao,
            m_vbo,
            m_ebo);

        Uninit();
        return false;
    }

    /*
     * 后续顶点属性和EBO配置都会记录到当前VAO。
     */
    glBindVertexArray(m_vao);

    /*
     * 上传四个顶点的数据。
     *
     * GL_STATIC_DRAW表示：
     * 顶点数据上传后基本不会再修改。
     */
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            kVideoVertices.size() *
            sizeof(VideoVertex)),
        kVideoVertices.data(),
        GL_STATIC_DRAW);

    /*
     * 上传六个顶点索引。
     *
     * EBO绑定会被记录在当前VAO中，
     * 因此不能在VAO仍然绑定时解除EBO绑定。
     */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            kVideoIndices.size() *
            sizeof(unsigned int)),
        kVideoIndices.data(),
        GL_STATIC_DRAW);

    /*
     * location = 0：顶点位置
     *
     * 对应Vertex Shader中的：
     * layout(location = 0) in vec2 vertex_position;
     */
    glVertexAttribPointer(
        0,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(VideoVertex),
        reinterpret_cast<const void*>(
            offsetof(VideoVertex, position_x)));

    glEnableVertexAttribArray(0);

    /*
     * location = 1：纹理坐标
     *
     * 对应Vertex Shader中的：
     * layout(location = 1) in vec2 texture_coordinate;
     */
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(VideoVertex),
        reinterpret_cast<const void*>(
            offsetof(VideoVertex, texture_x)));

    glEnableVertexAttribArray(1);

    /*
     * 顶点属性已经被VAO记录，可以解除普通VBO绑定。
     *
     * EBO不能在此处解除，因为它的绑定属于VAO状态。
     */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    m_initialized = true;

    LOG_INFO(
        "Video mesh initialized successfully: "
        "vao={}, vbo={}, ebo={}",
        m_vao,
        m_vbo,
        m_ebo);

    return true;
}

void VideoMesh::Uninit() noexcept {
    /*
     * 即使初始化只完成了一部分，也要根据ID释放资源，
     * 因此这里不能仅依赖m_initialized判断。
     */
    if (m_ebo != 0) {
        glDeleteBuffers(1, &m_ebo);
        m_ebo = 0;
    }

    if (m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }

    if (m_vao != 0) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }

    m_initialized = false;
}

void VideoMesh::Draw() const noexcept {
    if (!m_initialized) {
        return;
    }

    glBindVertexArray(m_vao);

    /*
     * 按EBO中的六个索引绘制两个三角形。
     * 六个索引最终组成一个矩形。
     */
    glDrawElements(
        GL_TRIANGLES,
        static_cast<GLsizei>(kVideoIndices.size()),
        GL_UNSIGNED_INT,
        nullptr);

    glBindVertexArray(0);
}

void VideoMesh::Bind() const noexcept {
    if (m_initialized) {
        glBindVertexArray(m_vao);
    }
}

void VideoMesh::Unbind() const noexcept {
    glBindVertexArray(0);
}

} // namespace RENDER