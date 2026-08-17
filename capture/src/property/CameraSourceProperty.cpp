#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ObsCaptureConstants.h"
#include "utils/logManager.h"
#include "CameraSourceProperty.h"

namespace {

std::optional<CAPTURE::CameraDeviceInfo>
FindCameraDevice(obs_properties_t* properties, const std::string& device_id) {
    if (!properties || device_id.empty()) {
        return std::nullopt;
    }

    obs_property_t* device_property = obs_properties_get(properties, CAPTURE::kDeviceProperty);
    if (!device_property) {
        return std::nullopt;
    }
            
    const std::size_t device_count = obs_property_list_item_count(device_property);
    
    for (size_t index = 0; index < device_count; index++) {
        const char* name = obs_property_list_item_name(device_property, index);

        // 获取实际设备 ID
        const char* id = obs_property_list_item_string(device_property, index);
        if (!id) {
            continue;
        }

        if (id == device_id) {
            CAPTURE::CameraDeviceInfo device;
            device.id = id;
            device.name = name;
            return device;
        }
    }
    return std::nullopt;
}

} // namespace

namespace CAPTURE {
std::shared_ptr<ICameraSourceProperty> ICameraSourceProperty::Create() {
    if (!obs_initialized()) {
        LOG_ERROR("Failed to create camera property: OBS is not initialized");
        return nullptr;
    }

    return std::make_shared<CameraSourceProperty>();
}

/*
    获取“源类型属性”时会给 V4L2 插件传入空的源实例数据。
    Ubuntu OBS 27 的 linux-v4l2 属性实现没有安全处理这个空指针，
    因此在插件内部崩溃
*/
// CameraSourceProperty::CameraSourceProperty() {
//     m_properties = obs_get_source_properties(kCameraSourceId);
//     if (!m_properties) {
//         LOG_ERROR("obs_get_source_properties:{} fail", kCameraSourceId);
//     }
// }

CameraSourceProperty::CameraSourceProperty() = default;

CaptureSourceType CameraSourceProperty::GetSourceType() const noexcept {
    return CaptureSourceType::kCST_Camera;
}

const char* CameraSourceProperty::GetSourceId() const noexcept {
    return kCameraSourceId;
}

std::vector<CameraDeviceInfo> CameraSourceProperty::EnumCameraDevices() const {
    std::vector<CameraDeviceInfo> devices;

    if (!m_properties) {
        LOG_ERROR("Failed to enumerate camera devices: properties are null");
        return devices;
    }

    obs_property_t* property = obs_properties_get(m_properties, kDeviceProperty);
    if (!property) {
        LOG_ERROR("Failed to enumerate camera devices");
        return devices;
    }

    const size_t dev_count = obs_property_list_item_count(property);
    devices.reserve(dev_count);

    for (size_t index = 0; index < dev_count; index++) {
        const char* id = obs_property_list_item_string(property, index);
        if (!id || *id == '\0') {
            continue;
        }

        const char* name = obs_property_list_item_name(property, index);

        devices.push_back({id, name ? name : id});
    }
    return devices;
}

bool CameraSourceProperty::SetVideoDevice(const std::string& device_id) {
    if (device_id.empty()) {
        LOG_ERROR("device_id empty");
        return false;
    }

    if (!m_properties) {
        LOG_ERROR("OBS properties are unavailable");
        return false;
    }

    const auto selected_device = FindCameraDevice(m_properties, device_id);
    if (!selected_device) {
        LOG_ERROR("Camera device was not found:{}", device_id);
        return false;
    }

    obs_data_t* setting = GetSetting();
    if (!setting) {
        LOG_ERROR("OBS settings are unavailable");
        return false;
    }

    // 设置本地保存的m_settings的device_id
    obs_data_set_string(setting, kDeviceProperty, device_id.c_str());

    /*
     * 通知 linux-v4l2：设备发生变化。
     * 该回调会根据新设备刷新 pixelformat 等关联属性，m_properties包含多个属性
     */
    obs_property_t* device_property = obs_properties_get(m_properties, kDeviceProperty);
    if (!device_property) {
        LOG_ERROR("Camera device property was not found");
        return false;
    }
    
    /*
    * 主动触发 device_id 属性注册的 modified callback。
    *
    * linux-v4l2 的 device_selected() 会从 setting 中读取新的
    * device_id，打开对应 V4L2 设备，修改 device_property 所属的整个 m_properties，刷新后续依赖属性
    * input / pixelformat / resolution / framerate 属性列表。
    */
    obs_property_modified(device_property, setting);

    // 如果采集源已经创建，立即应用设置;如果尚未创建，只保存 settings，之后创建源时使用
    if (m_obs_source) {
        obs_source_update(m_obs_source, setting);
    }

    LOG_INFO("Camera selected: name={}, id={}", selected_device->name, selected_device->id);
    return true;
}

std::optional<CameraDeviceInfo> CameraSourceProperty::GetCurrentDevice() const {
    obs_data_t* setting = GetSetting(); // 获取当前配置项
    if (!setting) {
        LOG_ERROR("OBS settings are unavailable");
        return std::nullopt;
    }
    
    const char* cur_device_id = obs_data_get_string(setting, kDeviceProperty); // 获取device_id
    if (!cur_device_id || *cur_device_id == '\0') {
        return std::nullopt;
    }

    // 根据device id查找设备信息
    const auto cur_device = FindCameraDevice(m_properties, cur_device_id);
    if (!cur_device) {
        LOG_ERROR("Current camera is no longer available: {}", cur_device_id);
        return std::nullopt;
    }

    return cur_device;
}

std::vector<CameraResolution> CameraSourceProperty::EnumResolutions() const {
    std::vector<CameraResolution> resolutions;

    if (!m_properties) {
        LOG_ERROR("Failed to enumerate camera resolution: properties are null");
        return resolutions;
    }

    obs_property_t* property = obs_properties_get(m_properties, kResolutionProperty);
    if (!property) {
        LOG_ERROR("Failed to enumerate camera resolution");
        return resolutions;
    }

    const size_t dev_count = obs_property_list_item_count(property);
    resolutions.reserve(dev_count);

    for (size_t index = 0; index < dev_count; index++) {
        const int64_t value = obs_property_list_item_int(property, index);

        if (value <= 0 || value > static_cast<std::int64_t>(UINT32_MAX)) {
            continue;
        }

        // OBS 27 实际使用一个 32 位整数存储分辨率，高低各占 16 位
        const auto packed = static_cast<std::uint32_t>(value);
        const auto width = static_cast<std::uint32_t>(packed >> 16U);
        const auto height = static_cast<std::uint32_t>(packed & 0xFFFFU);
        
        if (width == 0 || height == 0) {
            continue;
        }

        resolutions.push_back({width, height});
    }
    return resolutions;
}

bool CameraSourceProperty::SetResolution(const CameraResolution& resolution) {
    if (resolution.width == 0 || resolution.height == 0 || resolution.width > 0xFFFFU || resolution.height > 0xFFFFU) {
        LOG_ERROR("Failed to set camera resolution: invalid resolution");
        return false;
    }

    obs_data_t* settings = GetSetting();
    if (!settings) {
        LOG_ERROR("Failed to set camera resolution: settings are null");
        return false;
    }

    const std::uint32_t packed = (resolution.width) << 16U | (resolution.height & 0xFFFFU);
        

    obs_data_set_int(settings, kResolutionProperty, static_cast<std::int64_t>(packed));
    if (m_properties) {
        obs_property_t* resolution_property =
            obs_properties_get(m_properties, kResolutionProperty);

        if (resolution_property) {
            obs_property_modified(resolution_property, settings);
        }
    }

    if (m_obs_source) {
        obs_source_update(m_obs_source.Get(), settings);
    }

    return true;
}
    
std::optional<CameraResolution> CameraSourceProperty::GetCurrentResolution() const {
    obs_data_t* setting = GetSetting(); // 获取当前配置项
    if (!setting) {
        LOG_ERROR("OBS settings are unavailable");
        return std::nullopt;
    }
    
    const int64_t value = obs_data_get_int(setting, kResolutionProperty);
    if (value <= 0) {
        return std::nullopt;
    }

    const auto packed = static_cast<uint32_t>(value);
    CameraResolution resolution{static_cast<uint32_t>(packed >> 16U),
        static_cast<uint32_t>(packed & 0xFFFFU)};

    if (resolution.height == 0 || resolution.width == 0) {
        return std::nullopt;
    }
    return resolution;
}

std::vector<CameraVideoFormat> CameraSourceProperty::EnumVideoFormats() const {
    std::vector<CameraVideoFormat> formats;

    if (!m_properties) {
        LOG_ERROR("Failed to enumerate camera formats: properties are null");
        return formats;
    }

    obs_property_t* property = obs_properties_get(m_properties, kVideoFormatProperty);
    if (!property) {
        LOG_ERROR("Failed to enumerate camera formats");
        return formats;
    }

    const size_t dev_count = obs_property_list_item_count(property);
    formats.reserve(dev_count);

    for (size_t index = 0; index < dev_count; index++) {
        const int64_t value = obs_property_list_item_int(property, index);

        if (value < 0) {
            continue;
        }

        const char* name = obs_property_list_item_name(property, index);

        formats.push_back({value, name ? name : ""});
    }
    return formats;
}

bool CameraSourceProperty::SetVideoFormat(std::int64_t video_format) {
    if (video_format <= 0) {
        LOG_ERROR("Failed to set camera format: invalid format value");
        return false;
    }

    if (!m_properties) {
        LOG_ERROR("OBS properties are unavailable");
        return false;
    }

    obs_data_t* settings = GetSetting();
    if (!settings) {
        LOG_ERROR("Failed to set camera resolution: settings are null");
        return false;
    }

    obs_property_t* format_property =
        obs_properties_get(m_properties, kVideoFormatProperty);

    if (!format_property) {
        LOG_ERROR("Camera video format property was not found");
        return false;
    }

    obs_data_set_int(settings, kVideoFormatProperty, video_format);
    /*
     * 通知 linux-v4l2：像素格式发生变化。
     * 该回调会根据新格式刷新 resolution 属性列表。
     */
    obs_property_modified(format_property, settings);

    if (m_obs_source) {
        obs_source_update(m_obs_source.Get(), settings);
    }

    return true;
}
    
std::optional<CameraVideoFormat> CameraSourceProperty::GetCurrentVideoFormat() const {
    obs_data_t* setting = GetSetting();
    if (!setting) {
        LOG_ERROR("OBS settings are unavailable");
        return std::nullopt;
    }

    const int64_t cur_value = obs_data_get_int(setting, kVideoFormatProperty);
    if (cur_value <= 0) {
        return std::nullopt;
    }

    CameraVideoFormat cur_format{cur_value, ""};
    if (!m_properties) {
        return cur_format;
    }

    obs_property_t* property = obs_properties_get(m_properties, kVideoFormatProperty);

    if (!property) {
        return cur_format;
    }

    const size_t dev_count = obs_property_list_item_count(property);

    for (size_t index = 0; index < dev_count; index++) {
        const int64_t value = obs_property_list_item_int(property, index);

        if (value != cur_value) {
            continue;
        }

        const char* name = obs_property_list_item_name(property, index);
        if (name) {
            cur_format.name = name;
        }
        break;
    }
    return cur_format;
}


} //