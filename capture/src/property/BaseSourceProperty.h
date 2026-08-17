#pragma once

#include "ISource.h"
#include "ISourceProperty.h"
#include "utils/logManager.h"

#include <obs/obs.hpp>
#include <memory>
#include <utility>

namespace CAPTURE {
using OBSProperties = OBSPtr<obs_properties_t*, obs_properties_destroy>;
   
// 采集源属性的公共内部实现，负责某一采集设备的配置属性管理
template<typename AbstractSourceProperty>
class BaseSourceProperty : public AbstractSourceProperty {
public:
    BaseSourceProperty(const BaseSourceProperty&) = delete;

    BaseSourceProperty& operator=(const BaseSourceProperty&) = delete;

    BaseSourceProperty(BaseSourceProperty&&) = delete;

    BaseSourceProperty& operator=(BaseSourceProperty&&) = delete;

    // 获取当前 OBS settings, 返回的是由 BaseSourceProperty 持有的借用指针, 调用者不能调用 obs_data_release()
    obs_data* GetSetting() const {
        if (m_settings) {
            return m_settings;
        }

        if (m_obs_source) {
            // 该函数返回的 obs_data_t* 已经增加了一次引用计数，因此使用完成后必须调用 obs_data_release()。这里用 OBSDataAutoRelease 自动完成释放且不增加引用计数
            OBSDataAutoRelease source_settings = obs_source_get_settings(m_obs_source);
            if (source_settings) {
                m_settings = source_settings.Get(); // 取得原始指针
                return m_settings;
            }
        }

        /*
        * 尚未创建 OBS source 时，创建独立 settings。
        * 摄像头或麦克风属性可以先写入设置，之后再用这些
        * settings 创建 OBS source。
        * obs_data_create() 返回一个新的引用，最终必须调用 obs_data_release()
        */
        OBSDataAutoRelease settings = obs_data_create();
        if (!settings) {
            LOG_ERROR("Failed to create OBS source settings");
            return nullptr;
        }
        m_settings = settings.Get();
        return m_settings;
    }

    void SetSource(std::weak_ptr<ISource> source) noexcept override {
        auto shared_source = source.lock();

        // 空weak_ptr表示解除关联
        if (!shared_source) {
            ReleaseObsSource();
            m_weak_source.reset();
            return;
        }

        const char* source_name = shared_source->GetSourceName();
        if (!source_name || *source_name == '\0') {
            LOG_ERROR("source_name error");
            return;
        }

        /*
        * obs_get_source_by_name()返回增加过引用计数的指针，
        * OBSSourceAutoRelease负责释放这次临时引用。
        */
        OBSSourceAutoRelease obs_source =
            obs_get_source_by_name(source_name);

        if (!obs_source) {
            LOG_ERROR("Failed to find OBS source: {}", source_name);
            return;
        }

        /*
        * 先销毁旧属性。
        * 属性中的回调数据可能属于之前绑定的OBS源，因此必须在释放旧源前销毁。
        */
        m_properties = nullptr;

        /*
        * OBSSource赋值会增加引用计数。
        * obs_source离开当前作用域后释放临时引用，
        * m_obs_source仍然持有自己的引用。
        */
        m_obs_source = obs_source.Get();
        m_weak_source = std::move(source);

        // 保存当前真实源的配置
        OBSDataAutoRelease source_setting =
            obs_source_get_settings(m_obs_source.Get());

        if (source_setting) {
            m_settings = source_setting.Get();
        }

        /*
        * 必须通过真实的OBS源获取属性。
        * 不能再调用obs_get_source_properties("v4l2_input")。
        */
        m_properties =
            obs_source_properties(m_obs_source.Get());

        if (!m_properties) {
            LOG_WARN(
                "Failed to obtain properties for OBS source: {}",
                source_name
            );
        }
    }

    std::shared_ptr<ISource> GetSource() const noexcept override {
        return m_weak_source.lock();
    }

    // 解除当前 OBS source 关联
    void ReleaseObsSource() noexcept {
        if (m_obs_source) {
            OBSDataAutoRelease source_setting = obs_source_get_settings(m_obs_source);
            if (source_setting) {
                m_settings = source_setting.Get();
            }

            m_obs_source = nullptr;
        }
        m_weak_source.reset();
    }
protected:
    BaseSourceProperty() = default;

    ~BaseSourceProperty() override {
        ReleaseObsSource();
    }

protected:
    // 属性对象不拥有 ISource
    std::weak_ptr<ISource> m_weak_source;

    // properties 不是具体配置值，而是描述“有哪些配置项”，类似std::vector<Property>
    OBSProperties m_properties;

    // 保存当前已经创建的 OBS 输入源, 这是最终使用配置运行的 OBS Source
    OBSSource m_obs_source;

    // 当前属性配置, obs_data_t 可以理解为一个类似 JSON 的键值对象，创建Source时需要使用，
    // 原始指针需要手动释放，使用 OBSData 或 OBSDataAutoRelease 自动管理
    mutable OBSData m_settings;
        
};

} // namespace CAPTURE