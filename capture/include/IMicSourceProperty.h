#pragma once

#include "ISourceProperty.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace CAPTURE {

// 麦克风设备信息。
struct MicDeviceInfo {
    std::string id;
    std::string name;
};

// 麦克风属性接口
class CAPTURE_ENGINE_API IMicSourceProperty : public ISourceProperty {
public:
    ~IMicSourceProperty() override = default;

    // 创建一个新的麦克风属性对象
    static std::shared_ptr<IMicSourceProperty> Create();

    // 枚举当前可用的麦克风设备
    virtual std::vector<MicDeviceInfo> EnumMicDevices() const = 0;

    // 选择麦克风设备
    virtual bool SetMicDevice(const std::string& device_id) = 0;

    // 获取当前选择的麦克风
    virtual std::optional<MicDeviceInfo> GetCurrentMicDevice() const = 0;
};

} // namespace CAPTURE