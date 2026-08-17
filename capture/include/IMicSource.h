#pragma once

#include "ISource.h"
#include <memory>

namespace CAPTURE {

class IMicSourceProperty;

// 麦克风采集源接口
class CAPTURE_ENGINE_API IMicSource : public ISource {
public:
    ~IMicSource() override = default;

    /**
     * 获取该麦克风源对应的属性对象
     * 属性对象与当前麦克风源一一对应，可用于枚举和切换
     * 麦克风设备。
     */
    virtual std::shared_ptr<IMicSourceProperty> GetProperty() const noexcept = 0;
};

} // namespace CAPTURE