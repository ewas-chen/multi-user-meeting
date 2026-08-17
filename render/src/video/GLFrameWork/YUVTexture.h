#pragma once

#include "RenderDefine.h"

#include <cstdint>
#include <memory>

namespace RENDER {

/**
 * @brief 管理I420视频帧对应的三张OpenGL纹理
 *
 * I420由三个独立平面组成：
 *
 * Y平面：完整分辨率，保存亮度；
 * U平面：宽高各为Y的一半，保存色度；
 * V平面：宽高各为Y的一半，保存色度。
 *
 * 例如640x480的I420视频：
 *
 * Y纹理：640x480
 * U纹理：320x240
 * V纹理：320x240
 *
 * 本类只负责纹理的创建、更新、绑定和释放，
 * 不负责Shader转换和矩形绘制。
 *
 * 所有成员函数都必须在持有有效OpenGL上下文的
 * 渲染线程中调用。
 */
class RENDER_ENGINE_LOCAL YUVTexture final {
public:
    YUVTexture() = default;
    ~YUVTexture() noexcept;

    YUVTexture(const YUVTexture&) = delete;
    YUVTexture& operator=(const YUVTexture&) = delete;
    YUVTexture(YUVTexture&&) = delete;
    YUVTexture& operator=(YUVTexture&&) = delete;

    /**
     * @brief 创建Y、U、V三张OpenGL纹理
     *
     * 如果当前纹理尺寸与传入尺寸相同，直接返回成功。
     * 如果尺寸发生变化，会重新创建纹理。
     *
     * @param width  I420视频宽度，必须大于0且为偶数
     * @param height I420视频高度，必须大于0且为偶数
     */
    bool Initialize(int width, int height);

    /**
     * @brief 释放Y、U、V三张OpenGL纹理
     *
     * 调用时必须保证OpenGL上下文仍然有效。
     */
    void Uninit() noexcept;

    /**
     * @brief 将采集模块产生的I420Frame上传到OpenGL纹理
     *
     * 如果帧尺寸发生变化，会自动重新创建纹理。
     *
     * 这里直接使用CAPTURE::I420Frame的别名，
     * 不复制成渲染模块自己的帧结构。
     */
    bool UpdateYUVData(
        const std::shared_ptr<I420Frame>& frame);

    /**
     * @brief 将三个连续的I420平面上传到OpenGL纹理
     *
     * 该重载主要供内部调用，也可以用于后续解码模块
     * 直接提供Y、U、V平面数据。
     */
    bool UpdateYUVData(
        const std::uint8_t* y_data,
        const std::uint8_t* u_data,
        const std::uint8_t* v_data,
        int width,
        int height);

    /**
     * @brief 把Y、U、V纹理绑定到指定纹理单元
     *
     * 默认绑定关系：
     *
     * Y纹理 -> 纹理单元0
     * U纹理 -> 纹理单元1
     * V纹理 -> 纹理单元2
     *
     * 纹理单元编号必须与VideoShader::SetYUVTextures()
     * 中设置的编号保持一致。
     */
    void BindTextures(
        int y_texture_unit = 0,
        int u_texture_unit = 1,
        int v_texture_unit = 2) const noexcept;

    /**
     * @brief 解除指定纹理单元上的Y、U、V纹理绑定
     *
     * 参数必须与前一次BindTextures()使用的编号一致。
     */
    void UnbindTextures(
        int y_texture_unit = 0,
        int u_texture_unit = 1,
        int v_texture_unit = 2) const noexcept;

    /**
     * @brief 判断纹理资源是否创建完成
     */
    [[nodiscard]]
    bool IsInitialized() const noexcept {
        return m_initialized;
    }

    [[nodiscard]]
    unsigned int GetYTextureID() const noexcept {
        return m_y_texture_id;
    }

    [[nodiscard]]
    unsigned int GetUTextureID() const noexcept {
        return m_u_texture_id;
    }

    [[nodiscard]]
    unsigned int GetVTextureID() const noexcept {
        return m_v_texture_id;
    }

private:
    /**
     * @brief 帧尺寸变化时重新创建纹理
     */
    bool ResizeIfNeeded(int width, int height);

private:
    bool m_initialized{false};

    // 当前Y平面的宽度和高度
    int m_width{0};
    int m_height{0};

    // 三个I420平面对应的OpenGL纹理对象
    unsigned int m_y_texture_id{0};
    unsigned int m_u_texture_id{0};
    unsigned int m_v_texture_id{0};
};

} // namespace RENDER