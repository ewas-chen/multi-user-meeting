#include "VideoShader.h"

#include "ShaderSourceCodes.h"
#include "utils/logManager.h"

#include <glad/glad.h>

#include <vector>

namespace RENDER
{
VideoShader::~VideoShader() noexcept {
    Uninit();
}

// 把 ShaderSourceCodes.h 中两段 GLSL 文本编译、链接成 GPU 可以执行的程序，
// 并提前找到运行时需要设置的参数
bool VideoShader::Initialize() {
    if (IsInitialized()) {
        return true;
    }

    /*
     * 调用gladLoadGL()前，当前线程必须已经绑定有效的OpenGL上下文。
     * 不使用全局static标记，避免第一次加载失败后影响后续重新初始化。
     * glab查询OpenGL驱动函数地址后期调用
     */
    if (!gladLoadGL()) {
        LOG_ERROR("Failed to load OpenGL functions");
        return false;
    }

    /*
        CompileShader
        创建 Shader 对象。
        设置 Shader 源代码。
        编译源代码。
        查询是否编译成功。
    */
    const unsigned int vertex_shader = CompileShader(GL_VERTEX_SHADER, kI420VertexShaderSource, "I420 vertex shader");
    if (vertex_shader == 0) {
        return false;
    }

    const unsigned int fragment_shader = CompileShader(GL_FRAGMENT_SHADER, kI420FragmentShaderSource, "I420 fragment shader");
    if (fragment_shader == 0) {
        glDeleteShader(vertex_shader);
        return false;
    }  
        
    const unsigned int program_id = LinkProgram(vertex_shader, fragment_shader);
        
     /*
     * Shader已经被链接进Program。
     * 链接完成后，不再需要单独保留顶点Shader和片段Shader。
     */
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    if (program_id == 0) {
        return false;
    }
    
    // C++ 不能直接通过变量名不断设置 Shader 参数，而是先通过名字查询它在 Program 中的位置
    const int y_texture_location = glGetUniformLocation(program_id, "y_texture");
        
    const int u_texture_location = glGetUniformLocation(program_id, "u_texture");

    const int v_texture_location = glGetUniformLocation(program_id, "v_texture");

    const int mirror_horizontal_location = glGetUniformLocation(program_id, "mirror_horizontal");
        
    // 找不到uniform通常说明Shader源码中的名称与C++代码不一致，不能继续初始化
    if (y_texture_location < 0 || u_texture_location < 0 ||
        v_texture_location < 0 || mirror_horizontal_location < 0) {
        LOG_ERROR("Failed to obtain shader uniform locations: "
            "y={}, u={}, v={}, mirror={}",
            y_texture_location, u_texture_location,
            v_texture_location, mirror_horizontal_location);

        glDeleteProgram(program_id);
        return false;
    }

    m_program_id = program_id;
    m_y_texture_location = y_texture_location;
    m_u_texture_location = u_texture_location;
    m_v_texture_location = v_texture_location;
    m_mirror_horizontal_location = mirror_horizontal_location;
    
    /*
     * 设置默认纹理单元：
     * Y -> GL_TEXTURE0
     * U -> GL_TEXTURE1
     * V -> GL_TEXTURE2
     */
    Use();
    SetYUVTextures(0, 1, 2); // glUniform1i() 修改的是“当前正在使用的 Shader Program”
    SetMirrorHorizontal(false);
    Unuse();

    LOG_INFO("Video shader initialized successfully, program={}", m_program_id);

    return true;
}

void VideoShader::Uninit() noexcept {
    if (m_program_id != 0) {
        glDeleteProgram(m_program_id);
        m_program_id = 0;
    }

    m_y_texture_location = -1;
    m_u_texture_location = -1;
    m_v_texture_location = -1;
    m_mirror_horizontal_location = -1;
}

void VideoShader::Use() const noexcept {
    if (m_program_id != 0) {
        glUseProgram(m_program_id);
    }
}

void VideoShader::Unuse() const noexcept {
    glUseProgram(0);
}

void VideoShader::SetYUVTextures(int y_texture_unit,int u_texture_unit,int v_texture_unit) const noexcept {
    if (!IsInitialized()) {
        return;
    }

    /*
     * 这里设置的是纹理单元编号0、1、2，
     * 不是GL_TEXTURE0、GL_TEXTURE1、GL_TEXTURE2的枚举值。
     */
    glUniform1i(m_y_texture_location, y_texture_unit);

    glUniform1i(m_u_texture_location, u_texture_unit);

    glUniform1i(m_v_texture_location, v_texture_unit);
        
}

void VideoShader::SetMirrorHorizontal(bool mirror) const noexcept {
    if (!IsInitialized()) {
        return;
    }

    glUniform1i(m_mirror_horizontal_location, mirror ? GL_TRUE : GL_FALSE);
}

unsigned int VideoShader::CompileShader(unsigned int shader_type, std::string_view source, const char* shader_name) {
    if (source.empty()) {
        LOG_ERROR("Failed to compile {}: source is empty",
            shader_name ? shader_name : "shader");

        return 0;
    }

    const unsigned int shader = glCreateShader(shader_type);
    if (shader == 0) {
        LOG_ERROR("Failed to create {}",shader_name ? shader_name : "shader");
        return 0;
    }

    const char* source_data = source.data();
    const int source_length = static_cast<int>(source.size());
        
    /*
     * 显式传入字符串长度，因此string_view不要求以'\0'结尾。
     */
    glShaderSource(shader, 1, &source_data, &source_length);
        
    glCompileShader(shader);

    int compile_success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_success);

    if (compile_success == GL_TRUE) {
        return shader;
    }

    int log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

    if (log_length > 0) {
        std::vector<char> error_log(static_cast<std::size_t>(log_length), '\0');

        glGetShaderInfoLog(shader, log_length, nullptr, error_log.data());

        LOG_ERROR("Failed to compile {}: {}",
            shader_name ? shader_name : "shader",
            error_log.data());
            
    } else {
        LOG_ERROR( "Failed to compile {}",
            shader_name ? shader_name : "shader");
    }

    glDeleteShader(shader);
    return 0;
}
    
unsigned int VideoShader::LinkProgram(unsigned int vertex_shader, unsigned int fragment_shader) {
    if (vertex_shader == 0 || fragment_shader == 0) {
        LOG_ERROR("Failed to link shader program: invalid shader");
        return 0;
    }

    if (vertex_shader == 0 || fragment_shader == 0) {
        LOG_ERROR("Failed to link shader program: invalid shader");

        return 0;
    }

    const unsigned int program_id = glCreateProgram();
    if (program_id == 0) {
        LOG_ERROR("Failed to create shader program");
        return 0;
    }

    glAttachShader(program_id, vertex_shader);
    glAttachShader(program_id, fragment_shader);
    glLinkProgram(program_id);

    int link_success = GL_FALSE;
    glGetProgramiv(program_id, GL_LINK_STATUS, &link_success);

    if (link_success == GL_TRUE) {
        /*
         * Program已经完成链接，不再需要保持Shader附加状态。
         * Shader对象随后由Initialize()删除。
         */
        glDetachShader(program_id, vertex_shader);
        glDetachShader(program_id, fragment_shader);

        return program_id;
    }

    int log_length = 0;
    glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &log_length);

    if (log_length > 0) {
        std::vector<char> error_log(static_cast<std::size_t>(log_length), '\0');

        glGetProgramInfoLog(program_id, log_length,nullptr,error_log.data());

        LOG_ERROR("Failed to link shader program: {}",
            error_log.data());
            
    } else {
        LOG_ERROR("Failed to link shader program");
    }

    glDeleteProgram(program_id);
    return 0;
}
    
} // namespace RENDER


