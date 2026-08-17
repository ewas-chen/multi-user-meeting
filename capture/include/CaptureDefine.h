#pragma once

#include <cstdint>

/*
 * Linux ELF 动态库符号可见性。
 *
 * CAPTURE_ENGINE_API：
 *   对外公开的类、函数和数据类型。
 *
 * CAPTURE_ENGINE_LOCAL：
 *   仅供采集模块内部使用的符号。
 *
 * 建议配合编译选项 -fvisibility=hidden 使用。
 * 
    visibility 用来控制该符号是否出现在 Linux 动态库的对外符号表中。
    visibility("default")：对外可见，其他程序链接 .so 时可以使用。
    visibility("hidden")：仅动态库内部可见，不允许其他模块直接链接。
 */
#define CAPTURE_ENGINE_API __attribute__((visibility("default")))
#define CAPTURE_ENGINE_LOCAL __attribute__((visibility("hidden")))

namespace CAPTURE
{

// 采集源类型
enum class CaptureSourceType : std::uint8_t
{
    kCST_Unknown = 0,
    kCST_Mic     = 1,
    kCST_Camera  = 2
};

} // namespace CAPTURE
