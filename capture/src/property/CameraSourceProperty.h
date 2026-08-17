#pragma once

#include "BaseSourceProperty.h"
#include "ICameraSourceProperty.h"

namespace CAPTURE {

class CameraSourceProperty final : public BaseSourceProperty<ICameraSourceProperty> {
public:
    CameraSourceProperty();
    ~CameraSourceProperty() override = default;

    CameraSourceProperty(const CameraSourceProperty&) = delete;
    CameraSourceProperty& operator=(const CameraSourceProperty&) = delete;
    CameraSourceProperty(CameraSourceProperty&&) = delete;
    CameraSourceProperty& operator=(CameraSourceProperty&&) = delete;

    CaptureSourceType GetSourceType() const noexcept override;
    const char* GetSourceId() const noexcept override;

    std::vector<CameraDeviceInfo> EnumCameraDevices() const override;

    bool SetVideoDevice(const std::string& device_id) override;

    std::optional<CameraDeviceInfo> GetCurrentDevice() const override;

    std::vector<CameraResolution> EnumResolutions() const override;
    
    // 设置分辨率
    bool SetResolution(const CameraResolution& resolution) override;
        
    std::optional<CameraResolution> GetCurrentResolution() const override;
    
    std::vector<CameraVideoFormat> EnumVideoFormats() const override;

    bool SetVideoFormat(std::int64_t video_format) override;
        
    std::optional<CameraVideoFormat> GetCurrentVideoFormat() const override;
    
};

} // namespace CAPTURE