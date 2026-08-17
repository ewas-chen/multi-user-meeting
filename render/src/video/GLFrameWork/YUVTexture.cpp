#include "YUVTexture.h"

#include "utils/logManager.h"

#include <glad/glad.h>

#include <array>

namespace RENDER {

YUVTexture::~YUVTexture() noexcept {
    Uninit();
}

bool YUVTexture::Initialize(
    int width,
    int height) {

    if (width <= 0 || height <= 0 ||
        (width % 2) != 0 ||
        (height % 2) != 0) {

        LOG_ERROR(
            "Failed to initialize YUV texture: "
            "invalid I420 size {}x{}",
            width,
            height);

        return false;
    }

    if (m_initialized &&
        m_width == width &&
        m_height == height) {

        return true;
    }

    /*
     * 分辨率改变时，释放原来的纹理。
     *
     * Uninit()也可以清理只创建了一部分的纹理，
     * 因此不依赖m_initialized判断。
     */
    Uninit();

    std::array<unsigned int, 3> texture_ids{};
    glGenTextures(
        static_cast<GLsizei>(texture_ids.size()),
        texture_ids.data());

    m_y_texture_id = texture_ids[0];
    m_u_texture_id = texture_ids[1];
    m_v_texture_id = texture_ids[2];

    if (m_y_texture_id == 0 ||
        m_u_texture_id == 0 ||
        m_v_texture_id == 0) {

        LOG_ERROR(
            "Failed to create I420 textures: "
            "y={}, u={}, v={}",
            m_y_texture_id,
            m_u_texture_id,
            m_v_texture_id);

        Uninit();
        return false;
    }

    const std::array<unsigned int, 3> textures{
        m_y_texture_id,
        m_u_texture_id,
        m_v_texture_id
    };

    const std::array<int, 3> texture_widths{
        width,
        width / 2,
        width / 2
    };

    const std::array<int, 3> texture_heights{
        height,
        height / 2,
        height / 2
    };

    for (std::size_t index = 0;
         index < textures.size();
         ++index) {

        glBindTexture(
            GL_TEXTURE_2D,
            textures[index]);

        /*
         * U、V纹理的分辨率只有Y纹理的一半。
         * 使用线性过滤，让OpenGL在采样时自动插值。
         */
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MIN_FILTER,
            GL_LINEAR);

        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAG_FILTER,
            GL_LINEAR);

        /*
         * 视频纹理超出边缘时使用边缘像素，
         * 避免纹理重复造成画面边缘颜色异常。
         */
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_S,
            GL_CLAMP_TO_EDGE);

        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_T,
            GL_CLAMP_TO_EDGE);

        /*
         * GL_R8：
         * 每个像素只有一个8位通道。
         *
         * Y、U、V每个平面都只需要一个通道，
         * 不需要创建RGB三通道纹理。
         */
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_R8,
            texture_widths[index],
            texture_heights[index],
            0,
            GL_RED,
            GL_UNSIGNED_BYTE,
            nullptr);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    m_width = width;
    m_height = height;
    m_initialized = true;

    LOG_INFO(
        "I420 textures initialized: "
        "size={}x{}, y={}, u={}, v={}",
        width,
        height,
        m_y_texture_id,
        m_u_texture_id,
        m_v_texture_id);

    return true;
}

void YUVTexture::Uninit() noexcept {
    if (m_y_texture_id != 0) {
        glDeleteTextures(
            1,
            &m_y_texture_id);

        m_y_texture_id = 0;
    }

    if (m_u_texture_id != 0) {
        glDeleteTextures(
            1,
            &m_u_texture_id);

        m_u_texture_id = 0;
    }

    if (m_v_texture_id != 0) {
        glDeleteTextures(
            1,
            &m_v_texture_id);

        m_v_texture_id = 0;
    }

    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

bool YUVTexture::UpdateYUVData(
    const std::shared_ptr<I420Frame>& frame) {

    if (!frame) {
        LOG_ERROR(
            "Failed to update I420 textures: "
            "frame is null");

        return false;
    }

    if (!frame->IsValid()) {
        LOG_ERROR(
            "Failed to update I420 textures: "
            "frame is invalid");

        return false;
    }

    return UpdateYUVData(
        frame->data[0].get(),
        frame->data[1].get(),
        frame->data[2].get(),
        frame->width,
        frame->height);
}

bool YUVTexture::UpdateYUVData(
    const std::uint8_t* y_data,
    const std::uint8_t* u_data,
    const std::uint8_t* v_data,
    int width,
    int height) {

    if (!y_data || !u_data || !v_data) {
        LOG_ERROR(
            "Failed to update I420 textures: "
            "plane data is null");

        return false;
    }

    if (width <= 0 || height <= 0 ||
        (width % 2) != 0 ||
        (height % 2) != 0) {

        LOG_ERROR(
            "Failed to update I420 textures: "
            "invalid size {}x{}",
            width,
            height);

        return false;
    }

    if (!ResizeIfNeeded(width, height)) {
        LOG_ERROR(
            "Failed to resize I420 textures: {}x{}",
            width,
            height);

        return false;
    }

    /*
     * I420每个像素只有1字节。
     *
     * OpenGL默认要求每行数据按4字节对齐。
     * 某些视频宽度不能被4整除，因此这里设置为1字节对齐，
     * 防止OpenGL错误地跳过每行末尾的数据。
     */
    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        1);

    /*
     * 更新Y平面：完整分辨率。
     *
     * glTexSubImage2D只更新已创建纹理中的内容，
     * 不会每帧重新创建纹理对象。
     */
    glBindTexture(
        GL_TEXTURE_2D,
        m_y_texture_id);

    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        width,
        height,
        GL_RED,
        GL_UNSIGNED_BYTE,
        y_data);

    /*
     * 更新U平面：宽高分别为Y的一半。
     */
    glBindTexture(
        GL_TEXTURE_2D,
        m_u_texture_id);

    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        width / 2,
        height / 2,
        GL_RED,
        GL_UNSIGNED_BYTE,
        u_data);

    /*
     * 更新V平面：宽高分别为Y的一半。
     */
    glBindTexture(
        GL_TEXTURE_2D,
        m_v_texture_id);

    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        width / 2,
        height / 2,
        GL_RED,
        GL_UNSIGNED_BYTE,
        v_data);

    glBindTexture(GL_TEXTURE_2D, 0);

    /*
     * 恢复OpenGL默认的4字节对齐方式，
     * 避免影响渲染模块以外的纹理上传操作。
     */
    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        4);

    return true;
}

void YUVTexture::BindTextures(
    int y_texture_unit,
    int u_texture_unit,
    int v_texture_unit) const noexcept {

    if (!m_initialized ||
        y_texture_unit < 0 ||
        u_texture_unit < 0 ||
        v_texture_unit < 0) {

        return;
    }

    /*
     * 将Y纹理绑定到对应纹理单元。
     */
    glActiveTexture(
        static_cast<GLenum>(
            GL_TEXTURE0 + y_texture_unit));

    glBindTexture(
        GL_TEXTURE_2D,
        m_y_texture_id);

    /*
     * 将U纹理绑定到对应纹理单元。
     */
    glActiveTexture(
        static_cast<GLenum>(
            GL_TEXTURE0 + u_texture_unit));

    glBindTexture(
        GL_TEXTURE_2D,
        m_u_texture_id);

    /*
     * 将V纹理绑定到对应纹理单元。
     */
    glActiveTexture(
        static_cast<GLenum>(
            GL_TEXTURE0 + v_texture_unit));

    glBindTexture(
        GL_TEXTURE_2D,
        m_v_texture_id);

    /*
     * 恢复到默认纹理单元，避免影响后续OpenGL操作。
     */
    glActiveTexture(GL_TEXTURE0);
}

void YUVTexture::UnbindTextures(
    int y_texture_unit,
    int u_texture_unit,
    int v_texture_unit) const noexcept {

    if (y_texture_unit < 0 ||
        u_texture_unit < 0 ||
        v_texture_unit < 0) {

        return;
    }

    glActiveTexture(
        static_cast<GLenum>(
            GL_TEXTURE0 + y_texture_unit));

    glBindTexture(
        GL_TEXTURE_2D,
        0);

    glActiveTexture(
        static_cast<GLenum>(
            GL_TEXTURE0 + u_texture_unit));

    glBindTexture(
        GL_TEXTURE_2D,
        0);

    glActiveTexture(
        static_cast<GLenum>(
            GL_TEXTURE0 + v_texture_unit));

    glBindTexture(
        GL_TEXTURE_2D,
        0);

    glActiveTexture(GL_TEXTURE0);
}

bool YUVTexture::ResizeIfNeeded(
    int width,
    int height) {

    if (m_initialized &&
        m_width == width &&
        m_height == height) {

        return true;
    }

    return Initialize(width, height);
}

} // namespace RENDER