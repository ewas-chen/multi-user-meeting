#pragma once
#include "CaptureDefine.h"

namespace CAPTURE {

// 所有采集源的公共抽象接口
class CAPTURE_ENGINE_API ISource {
public:
    virtual ~ISource() = default;

    // 获取采集源类型
    virtual CaptureSourceType GetSourceType() const noexcept = 0;

    // 获取采集源名称
    virtual const char* GetSourceName() const noexcept = 0;

    // 获取底层采集源类型标识，在 OBS 实现中，该值可以对应 OBS Source ID
    virtual const char* GetSourceId() const noexcept = 0;

    // 获取视频源当前输出宽度
    virtual int32_t GetSourceWidth() const noexcept = 0;

    //获取视频源当前输出高度
    virtual int32_t GetSourceHeight() const noexcept = 0;
};
} // namespace CAPTURE

