#pragma once

#include "CaptureDefine.h"
#include "ISourceProperty.h"
#include <vector>
#include <string>
#include <optional>

namespace CAPTURE {

// 摄像头设备信息
struct CameraDeviceInfo {
    std::string id;
    std::string name;
};

// 摄像头输入分辨率
struct CameraResolution {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    [[nodiscard]]
    constexpr bool IsValid() const noexcept {
        return width > 0 && height > 0;
    }

    constexpr bool operator==(const CameraResolution& other) const noexcept {
        return width == other.width && height == other.height;
    }
};

// 摄像头输入格式, 这里表示摄像头原始输入格式
struct CameraVideoFormat {
    // OBS/V4L2 属性对应的实际值，而不是列表下标。
    std::int64_t value = 0;

    // 用于界面显示，例如 "MJPEG"、"YUYV 4:2:2"。
    std::string name;
};


class CAPTURE_ENGINE_API ICameraSourceProperty : public ISourceProperty {
public:
    ~ICameraSourceProperty() override = default;

    // 创建一个新的摄像头属性对象
    // 每次调用都应返回独立对象，不能使用全局单例，因为不同摄像头源需要保存各自的设备配置
    // 必须在 CaptureEngine 完成 OBS 初始化及模块加载后调用
    static std::shared_ptr<ICameraSourceProperty> Create();

    // 枚举当前可用的摄像头设备
    virtual std::vector<CameraDeviceInfo> EnumCameraDevices() const = 0;

    // 选择摄像头设备
    virtual bool SetVideoDevice(const std::string& device_id) = 0;
        
    // 获取当前选择的摄像头
    virtual std::optional<CameraDeviceInfo> GetCurrentDevice() const = 0;

    // 枚举当前摄像头支持的分辨率
    virtual std::vector<CameraResolution> EnumResolutions() const = 0;

    // 设置摄像头输入分辨率
    virtual bool SetResolution(const CameraResolution& resolution) = 0;

    // 获取当前摄像头输入分辨率
    virtual std::optional<CameraResolution> GetCurrentResolution() const = 0;

    // 枚举当前摄像头支持的输入格式
    virtual std::vector<CameraVideoFormat> EnumVideoFormats() const = 0;

    // 设置摄像头输入格式
    virtual bool SetVideoFormat(std::int64_t format_value) = 0;
       
    // 获取当前摄像头输入格式
    virtual std::optional<CameraVideoFormat> GetCurrentVideoFormat() const = 0;
};

} // namespace CAPTURE