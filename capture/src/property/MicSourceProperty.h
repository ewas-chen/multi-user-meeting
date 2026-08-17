#pragma once

#include "IMicSourceProperty.h"
#include "property/BaseSourceProperty.h"

#include <optional>
#include <string>
#include <vector>

namespace CAPTURE {

class MicSourceProperty final : public BaseSourceProperty<IMicSourceProperty> {
public:
    MicSourceProperty();
    ~MicSourceProperty() override = default;

    MicSourceProperty(const MicSourceProperty&) = delete;
    MicSourceProperty& operator=(const MicSourceProperty&) = delete;
    MicSourceProperty(MicSourceProperty&&) = delete;
    MicSourceProperty& operator=(MicSourceProperty&&) = delete;

public:
    [[nodiscard]]
    CaptureSourceType GetSourceType() const noexcept override;

    [[nodiscard]]
    const char* GetSourceId() const noexcept override;

    [[nodiscard]]
    std::vector<MicDeviceInfo> EnumMicDevices() const override;

    [[nodiscard]]
    bool SetMicDevice(const std::string& device_id) override;

    [[nodiscard]]
    std::optional<MicDeviceInfo> GetCurrentMicDevice() const override;
};

} // namespace CAPTURE