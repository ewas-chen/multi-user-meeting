#pragma once

/*
 * Linux ELF动态库符号可见性。
 *
 * VCE_API：
 *   标记需要向VideoConferenceEngine动态库外部公开的类、函数和数据。
 *
 * VCE_LOCAL：
 *   标记只允许会议引擎内部使用的实现符号。
 *
 * 建议配合编译选项-fvisibility=hidden使用。
 */
#define VCE_API __attribute__((visibility("default")))
#define VCE_LOCAL __attribute__((visibility("hidden")))