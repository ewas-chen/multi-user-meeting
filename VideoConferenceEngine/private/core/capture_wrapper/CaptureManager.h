#pragma once

#include "ICaptureDataCallback.h"
#include "VceTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CAPTURE {
class ICaptureEngine;
class ICameraSourceProperty;
class IMicSourceProperty;
} // namespace CAPTURE

namespace VCE {

/**
 * @brief 采集模块包装器
 *
 * CaptureManager负责：
 * - 管理CaptureEngine生命周期；
 * - 枚举和选择摄像头、麦克风设备；
 * - 创建和删除摄像头、麦克风采集源；
 * - 将采集帧转发给VceEngineImpl。
 *
 * 会议层和采集模块使用相同的I420Frame、AudioFrame类型，
 * 因此这里不进行音视频数据转换或复制。
 */
class CaptureManager
{
public:
    CaptureManager();
    ~CaptureManager();

    CaptureManager(const CaptureManager&) = delete;
    CaptureManager& operator=(const CaptureManager&) = delete;
    CaptureManager(CaptureManager&&) = delete;
    CaptureManager& operator=(CaptureManager&&) = delete;

    // ==================== 生命周期 ====================

    Result Initialize(
        int sample_rate,
        int channels,
        int video_width,
        int video_height,
        int video_fps);

    void Uninit();

    // ==================== 设备管理 ====================

    Result GetCameraDevices(
        std::vector<CameraDeviceInfo>& devices);

    Result GetMicrophoneDevices(
        std::vector<MicDeviceInfo>& devices);

    Result GetCurrentCameraDeviceId(
        std::string& camera_device_id);

    Result GetCurrentMicrophoneDeviceId(
        std::string& microphone_device_id);

    /**
     * @brief 选择摄像头设备
     *
     * 摄像头已经打开时立即更新当前采集源；
     * 摄像头尚未打开时保存设备ID，在创建采集源后应用。
     */
    Result UpdateCameraDevice(
        const std::string& camera_device_id);

    /**
     * @brief 配置已经打开的摄像头输入格式和分辨率
     *
     * CaptureManager::Initialize中的视频参数配置的是OBS输出，
     * 此接口负责把格式和分辨率应用到实际V4L2摄像头输入。
     */
    Result ConfigureCameraInput(
        const std::string& video_format_name,
        int width,
        int height);

    /**
     * @brief 选择麦克风设备
     *
     * 麦克风已经打开时立即更新当前采集源；
     * 麦克风尚未打开时保存设备ID，在创建采集源后应用。
     */
    Result UpdateMicrophoneDevice(
        const std::string& microphone_device_id);

    // ==================== 采集源管理 ====================

    Result OpenCamera();
    Result CloseCamera();

    Result OpenMic();
    Result CloseMic();

    /**
     * @brief 设置采集帧接收对象
     *
     * 使用weak_ptr保存回调对象，避免CaptureManager与
     * VceEngineImpl之间形成shared_ptr循环引用。
     */
    Result SetCaptureDataCallback(
        const std::shared_ptr<ICaptureDataCallback>& callback);

private:
    /**
     * @brief 创建指定类型的OBS采集源
     *
     * 创建成功后保存source_id与source_name之间的对应关系。
     */
    Result CreateCaptureSource(
        CaptureType type,
        int& source_id);

    /**
     * @brief 删除指定采集源
     */
    Result DestroyCaptureSource(
        int source_id);

    /**
     * @brief 获取当前摄像头源持有的属性对象
     */
    std::shared_ptr<CAPTURE::ICameraSourceProperty>
    GetCameraProperty() const;

    /**
     * @brief 获取当前麦克风源持有的属性对象
     */
    std::shared_ptr<CAPTURE::IMicSourceProperty>
    GetMicrophoneProperty() const;

    /**
     * @brief 接收CaptureEngine视频回调并转发给VceEngineImpl
     */
    void OnEngineVideoFrame(const std::shared_ptr<I420Frame>& frame);
        
    /**
     * @brief 接收CaptureEngine音频回调并转发给VceEngineImpl
     */
    void OnEngineAudioFrame(const std::shared_ptr<AudioFrame>& frame);

private:
    std::unique_ptr<CAPTURE::ICaptureEngine> m_capture_engine;

    /*
     * 保留源码的内部source_id管理方式。
     * key：会议层内部source_id
     * value：OBS源名称和会议层采集源类型
     */
    std::unordered_map<int, std::pair<std::string, CaptureType>> m_source_id_info_map;

    std::atomic_int m_next_source_id{1};

    int m_camera_source_id{-1};
    int m_microphone_source_id{-1};

    /*
     * 用户可能在打开设备前进行选择，因此需要保存待应用的设备ID。
     * 采集源创建成功后再将其设置到源所持有的属性对象中。
     */
    std::string m_selected_camera_device_id;
    std::string m_selected_microphone_device_id;

    /*
     * 保护采集引擎、采集源映射和设备选择状态。
     * 设备控制与Uninit不能同时修改这些对象。
     */
    mutable std::mutex m_state_mutex;

    /*
     * 只保护回调对象本身。
     * 实际执行外部回调前应先取得shared_ptr，然后立即释放该锁。
     */
    mutable std::mutex m_callback_mutex;
    std::weak_ptr<ICaptureDataCallback> m_capture_callback;

    std::atomic<bool> m_initialized{false};
};

} // namespace VCE