// 在 GPU 上运行的着色器代码，用来把采集模块输出的 I420 视频转换成屏幕上能显示的 RGB 图像
#pragma once

#include <string_view>

namespace RENDER
{
/**
 *  I420视频顶点Shader
 *
 * 顶点属性：
 * - location 0：顶点位置(屏幕上矩形四个顶点的位置)
 * - location 1：纹理坐标(纹理坐标用于指定视频图像的哪个位置应该贴到矩形顶点上)
 *
 * mirror_horizontal用于本地摄像头预览水平镜像。
 * 镜像直接在GPU中修改纹理坐标，不需要在CPU中翻转I420数据。
 */

// 这段顶点Shader的作用是告诉GPU:视频矩形画在屏幕什么位置，以及视频纹理的哪个位置对应矩形的哪个位置
inline constexpr std::string_view kI420VertexShaderSource = R"glsl(

#version 330 core // GLSL 3.30 Core 版本

layout(location = 0) in vec2 vertex_position;
layout(location = 1) in vec2 texture_coordinate;

out vec2 fragment_texture_coordinate;

uniform bool mirror_horizontal; // uniform:由 C++ 代码传给 Shader 的全局参数, C++ 可以在绘制前修改

void main()
{
    gl_Position = vec4(vertex_position, 0.0, 1.0); // 把当前顶点放到对应位置

    vec2 coordinate = texture_coordinate;

    /*
        只是改变纹理读取方向，不修改原始视频帧，
        也不需要在 CPU 上复制数据。通常本地摄像头预览需要镜像，远端用户视频不需要。
    */
    if (mirror_horizontal)
    {
        coordinate.x = 1.0 - coordinate.x;
    }

    fragment_texture_coordinate = coordinate;
}
)glsl";

/**
 * @brief I420转RGB片元Shader
 *
 * 输入纹理：
 * - y_texture：完整分辨率的Y平面。
 * - u_texture：宽高各为一半的U平面。
 * - v_texture：宽高各为一半的V平面。
 *
 * CaptureEngine当前输出：
 * - I420/YUV420P
 * - BT.709
 * - Full Range
 *
 * Full Range的Y值不需要执行：
 *
 *     (Y - 16) / 219
 *
 * U和V纹理采样值位于[0, 1]，减去0.5后转换到
 * 以0为中心的色度范围。
 */
inline constexpr std::string_view kI420FragmentShaderSource = R"glsl(
#version 330 core

in vec2 fragment_texture_coordinate;

out vec4 fragment_color;

uniform sampler2D y_texture;
uniform sampler2D u_texture;
uniform sampler2D v_texture;

void main()
{
    // 表示读取纹理的红色通道, 虽然叫红色通道，但这里的纹理是单通道纹理
    float y = texture(
        y_texture,
        fragment_texture_coordinate).r;

    float u = texture(
        u_texture,
        fragment_texture_coordinate).r - 0.5;

    float v = texture(
        v_texture,
        fragment_texture_coordinate).r - 0.5;

    /*
     * BT.709 Full Range YUV到RGB转换。
     */
    float red =
        y + 1.5748 * v;

    float green =
        y - 0.1873 * u
          - 0.4681 * v;

    float blue =
        y + 1.8556 * u;

    // 限制颜色范围
    vec3 rgb = clamp(
        vec3(red, green, blue),
        0.0,
        1.0);

    fragment_color = vec4(rgb, 1.0);
}
)glsl";



} // namespace RENDER
