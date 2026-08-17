#include "MicSourceProperty.h"
#include "ObsCaptureConstants.h"
#include "utils/logManager.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

std::optional<CAPTURE::MicDeviceInfo>
FindMicDevice(obs_properties_t* properties, const std::string& device_id) {
    if (!properties || device_id.empty()) {
        return std::nullopt;
    }

    // 从整个麦克风属性集合中，根据属性标识符找到“音频设备选择”这一项, 对麦克风而言，设备属性通常是一个列表类型(默认设备,USB 麦克风,内置麦克风)
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
            CAPTURE::MicDeviceInfo device;
            device.id = id;
            device.name = name;
            return device;
        }
    }
    return std::nullopt;
}

} // namespace

namespace CAPTURE {

std::shared_ptr<IMicSourceProperty> IMicSourceProperty::Create() {
    if (!obs_initialized()) {
        LOG_ERROR("Cannot create microphone property: OBS not init");
        return nullptr;
    }

    return std::make_shared<MicSourceProperty>();
}

// MicSourceProperty::MicSourceProperty() {
//     m_properties = obs_get_source_properties(kMicSourceId);
//     if (!m_properties) {
//         LOG_ERROR("Failed to get properties for OBS source:{}", kMicSourceId);
//         return;
//     }
// }

MicSourceProperty::MicSourceProperty() = default;

CaptureSourceType MicSourceProperty::GetSourceType() const noexcept {
    return CaptureSourceType::kCST_Mic;
}

const char* MicSourceProperty::GetSourceId() const noexcept {
    return kMicSourceId;
}

std::vector<MicDeviceInfo> MicSourceProperty::EnumMicDevices() const {
    std::vector<MicDeviceInfo> devices;

    if (!m_properties) {
        LOG_ERROR("OBS properties are unavailable");
        return devices;
    }

    obs_property_t* device_property = obs_properties_get(m_properties, kDeviceProperty);
    if (!device_property) {
        LOG_ERROR("OBS microphone property {} was not found", kDeviceProperty);
        return devices;
    }

    const size_t device_count = obs_property_list_item_count(device_property);
    devices.reserve(device_count);

    for (size_t index = 0; index < device_count; index++) {
        const char* name = obs_property_list_item_name(device_property, index);

        const char* id = obs_property_list_item_string(device_property, index);
        if (!id || *id == '\0') {
            continue;
        }

        MicDeviceInfo device;

        device.id = id;
        device.name = name ? name : "";

        LOG_INFO("Found microphone: name={}, id={}", device.name, device.id);

        devices.emplace_back(std::move(device));
    }
    return devices;
}

bool MicSourceProperty::SetMicDevice(const std::string& device_id) {
    if (device_id.empty()) {
        LOG_ERROR("device_id empty");
        return false;
    }

    if (!m_properties) {
        LOG_ERROR("OBS properties are unavailable");
        return false;
    }

    const auto selected_device = FindMicDevice(m_properties, device_id);
    if (!selected_device) {
        LOG_ERROR("Microphone device was not found:{}", device_id);
        return false;
    }

    obs_data_t* setting = GetSetting();
    if (!setting) {
        LOG_ERROR("OBS settings are unavailable");
        return false;
    }

    // 设置本地保存的m_settings
    obs_data_set_string(setting, kDeviceProperty, device_id.c_str());

    // 如果采集源已经创建，立即应用设置;如果尚未创建，只保存 settings，之后创建源时使用
    if (m_obs_source) {
        obs_source_update(m_obs_source, setting);
    }

    LOG_INFO("Microphone selected: name={}, id={}", selected_device->name, selected_device->id);
    return true;
}

std::optional<MicDeviceInfo> MicSourceProperty::GetCurrentMicDevice() const {
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
    const auto cur_device = FindMicDevice(m_properties, cur_device_id);
    if (!cur_device) {
        LOG_ERROR("Current microphone is no longer available: {}", cur_device_id);
        return std::nullopt;
    }

    return cur_device;
}

} // namespace CAPTURE