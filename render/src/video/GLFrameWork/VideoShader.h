#pragma once

#include "RenderDefine.h"

#include <string_view>

namespace RENDER
{
// 管理I420视频渲染使用的OpenGL Shader程序
/*
    * 主要职责：
    * 1. 编译顶点Shader和片段Shader；
    * 2. 将两个Shader链接为一个OpenGL程序；
    * 3. 缓存Y、U、V纹理及镜像参数的位置；
    * 4. 在视频绘制前启用Shader程序。
*/
class RENDER_ENGINE_LOCAL VideoShader final {
public:
    VideoShader() = default;
    ~VideoShader() noexcept;

    VideoShader(const VideoShader&) = delete;
    VideoShader& operator=(const VideoShader&) = delete;
    VideoShader(VideoShader&&) = delete;
    VideoShader& operator=(VideoShader&&) = delete;

    //  编译并链接I420视频渲染Shader
    bool Initialize();

    // 释放OpenGL Shader程序
    void Uninit() noexcept;

    // 启用当前Shader程序
    void Use() const noexcept;

    // 取消使用当前Shader程序
    void Unuse() const noexcept;

    // 设置Y、U、V纹理对应的OpenGL纹理单元(纹理单元编号可以理解成一个索引，Shader 后续通过这个索引找到绑定的纹理)
    void SetYUVTextures(
        int y_texture_unit = 0,
        int u_texture_unit = 1,
        int v_texture_unit = 2) const noexcept;

    // 设置是否水平镜像视频画面
    void SetMirrorHorizontal(bool mirror) const noexcept;

    // 判断Shader程序是否初始化成功
    // OpenGL 创建的资源通常用一个整数 ID 表示,是 OpenGL 资源句柄,ID 为 0 表示当前没有有效程序
    [[nodiscard]]
    bool IsInitialized() const noexcept {
        return m_program_id != 0;
    }

    // 获取OpenGL Shader程序ID
    [[nodiscard]]
    unsigned int GetProgramID() const noexcept {
        return m_program_id;
    }
private:
    // 编译一个顶点或片段Shader
    static unsigned int CompileShader(
        unsigned int shader_type,
        std::string_view source,
        const char* shader_name);

    // 将顶点Shader和片段Shader链接为一个程序
    static unsigned int LinkProgram(
        unsigned int vertex_shader,
        unsigned int fragment_shader);


private:
    // 链接完成的OpenGL Shader程序
    unsigned int m_program_id{0};

    // Y、U、V纹理采样器在Shader程序中的位置
    int m_y_texture_location{-1};
    int m_u_texture_location{-1};
    int m_v_texture_location{-1};

    // 水平镜像参数在Shader程序中的位置
    int m_mirror_horizontal_location{-1};

};

    
} // namespace RENDER
