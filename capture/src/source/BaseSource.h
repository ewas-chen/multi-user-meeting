#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <obs/obs.hpp>

#include "ISource.h"

namespace CAPTURE {

// BaseSource 持有一个属性对象，所以使用 shared_ptr<Property>
// enable_shared_from_this 的模板参数应该是“想通过当前 this 获得哪种对象的 shared_ptr”。
// 这里想获得采集源自己，因此参数必须是 BaseSource<...>，不能是它所持有的 Property。
template<class SourceInterface, class Property>
class BaseSource : public SourceInterface, public std::enable_shared_from_this<BaseSource<SourceInterface, Property>> {
public:
    ~BaseSource() override {
        /*
         * 属性对象可能被外部继续持有。
         * Source 销毁前解除属性对象与 OBS 源的关联。
         */
        if (m_property){
            m_property->SetSource(std::weak_ptr<ISource>{});
        }
    }

    BaseSource(const BaseSource&) = delete;
    BaseSource& operator=(const BaseSource&) = delete;
    BaseSource(BaseSource&&) = delete;
    BaseSource& operator=(BaseSource&&) = delete;

    bool Init(const std::string& source_name, const std::shared_ptr<Property>& property) {
        if (source_name.empty()) {
            LOG_ERROR("source_name empty");
            return false;
        }

        if (!property) {
            LOG_ERROR("property is null");
            return false;
        }

        if (m_obs_source) {
            LOG_ERROR("source is already initialized");
            return false;
        }

        // 指向当前对象的 shared_ptr
        auto self = this->weak_from_this().lock();
        if (!self) {
            LOG_ERROR("source is not managed by shared_ptr");
            return false;
        }

        const char* source_id = property->GetSourceId(); // 返回固定值
        if (!source_id || *source_id == '\0') {
            LOG_ERROR("OBS source id is empty");
            return false;
        }

        obs_data_t* settings = property->GetSetting(); // 初次调用返回obs默认setting
        if (!settings) {
            LOG_ERROR("settings are null");
            return false;
        }

        obs_source_t* obs_source = obs_source_create(source_id, source_name.c_str(), settings, nullptr);
        if (!obs_source) {
            LOG_ERROR("Failed to create OBS source {}, id={}", source_name, source_id);
            return false;
        }

        /*
         * OBSSource 赋值时会增加引用计数。
         * 赋值完成后释放 obs_source_create() 返回的原始引用。
         */
        m_obs_source = obs_source;
        obs_source_release(obs_source);

        // void SetSource(std::weak_ptr<ISource> source) noexcept override
        m_property = property;
        m_property->SetSource(std::static_pointer_cast<ISource>(self));

        return true;
    }

    CaptureSourceType GetSourceType() const noexcept override {
        if (!m_property) {
            return CaptureSourceType::kCST_Unknown;
        }

        return m_property->GetSourceType();
    }

    const char* GetSourceName() const noexcept override {
        if (!m_obs_source) {
            return "";
        }

        const char* name = obs_source_get_name(m_obs_source.Get());
        return name ? name : "";
    }

    const char* GetSourceId() const noexcept override {
        if (!m_obs_source) {
            return "";
        }

        const char* id = obs_source_get_id(m_obs_source.Get());
        return id ? id : "";
    }

    std::int32_t GetSourceWidth() const noexcept override {
        if (!m_obs_source) {
            return 0;
        }

        return obs_source_get_width(m_obs_source.Get());
    }

    std::int32_t GetSourceHeight() const noexcept override {
        if (!m_obs_source) {
            return 0;
        }

        return obs_source_get_height(m_obs_source.Get());
    }

    obs_source_t* GetObsSource() const noexcept {
        return m_obs_source.Get();
    }

protected:
    BaseSource() = default;

    OBSSource m_obs_source;
    std::shared_ptr<Property> m_property;

};
} // namespace