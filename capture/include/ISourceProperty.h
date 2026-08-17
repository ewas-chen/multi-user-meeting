#pragma once

#include "CaptureDefine.h"
#include "ISource.h"
#include <memory>

namespace CAPTURE {
class CAPTURE_ENGINE_API ISourceProperty {
public:
    virtual ~ISourceProperty() = default;

    // 获取该属性对应的采集源类型
    virtual CaptureSourceType GetSourceType() const noexcept = 0;

    // 获取底层采集源类型标识
    virtual const char* GetSourceId() const noexcept = 0;

    // 获取当前关联的采集源
    virtual std::shared_ptr<ISource> GetSource() const noexcept = 0;

    // 设置当前关联的采集源(属性对象只保存采集源的弱引用，不延长采集源的生命周期)
    virtual void SetSource(std::weak_ptr<ISource> source) noexcept = 0;

};

} // namespace CAPTURE