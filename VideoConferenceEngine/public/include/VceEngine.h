#pragma once

#include "VceGlobal.h"
#include "VceTypes.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace VCE {

class VceEngineImpl;
class IMeetingEventObserver;

/**
 * @brief 视频会议引擎公开接口
 *
 * VceEngine是提供给客户端使用的门面类，具体功能由内部的
 * VceEngineImpl完成。客户端不需要直接访问采集、传输、
 * 渲染和业务服务模块。
 */
class VCE_API VceEngine
{
public:
    /**
     * @brief 获取全局会议引擎实例
     */
    static std::shared_ptr<VceEngine> GetInstance();

    /**
     * @brief 释放会议引擎持有的全局实例
     *
     * 调用前应先执行UninitSubModel()。
     * 如果外部仍持有shared_ptr，实例会在最后一个引用释放后销毁。
     */
    static void ReleaseInstance();

    ~VceEngine();

    VceEngine(const VceEngine&) = delete;
    VceEngine& operator=(const VceEngine&) = delete;
    VceEngine(VceEngine&&) = delete;
    VceEngine& operator=(VceEngine&&) = delete;

private:
    VceEngine();

public:
    // ==================== 引擎生命周期 ====================

    /**
     * @brief 初始化采集、传输、渲染和业务服务模块
     * @param config 引擎音视频参数及业务服务配置
     */
    Result InitSubModel(const EngineConfig& config = {});

    /**
     * @brief 停止并释放所有子模块
     */
    Result UninitSubModel();

    // ==================== 用户服务 ====================

    Result RegisterUser(
        const std::string& user_name,
        const std::string& password);

    Result LoginUser(
        const std::string& user_name,
        const std::string& password);

    // ==================== 会议管理 ====================

    Result CreateMeeting(
        const CreateMeetingInfo& request,
        CreateMeetingResponse& response);

    Result JoinMeeting(
        const MeetingBriefInfo& request,
        JoinMeetingResponse& response);

    Result LeaveMeeting(
        const MeetingBriefInfo& request);

    Result EndMeeting(
        const MeetingBriefInfo& request);

    Result GetMeetingList(
        const GetMeetingListRequest& request,
        GetMeetingListResponse& response);

    // ==================== 采集设备管理 ====================

    Result GetCameraDevices(
        std::vector<CameraDeviceInfo>& devices);

    Result GetMicrophoneDevices(
        std::vector<MicDeviceInfo>& devices);

    Result GetCurrentCameraDeviceId(
        std::string& camera_device_id);

    Result GetCurrentMicrophoneDeviceId(
        std::string& microphone_device_id);

    Result UpdateCameraDevice(
        const std::string& camera_device_id);

    Result UpdateMicrophoneDevice(
        const std::string& microphone_device_id);

    /**
     * @brief 打开摄像头并开始发布本地视频
     */
    Result OpenCamera();

    /**
     * @brief 停止本地视频发布并关闭摄像头
     */
    Result CloseCamera();

    /**
     * @brief 打开麦克风并开始发布本地音频
     */
    Result OpenMic();

    /**
     * @brief 停止本地音频发布并关闭麦克风
     */
    Result CloseMic();

    // ==================== 渲染用户管理 ====================

    Result AddUser(
        const std::string& user_name,
        bool is_local);

    Result RemoveUser(
        const std::string& user_name);

    /**
     * @brief 在当前OpenGL上下文中渲染指定用户
     *
     * 该函数应由拥有OpenGL上下文的渲染线程调用。
     */
    Result RenderUser(
        const std::string& user_name);

    Result UpdateUserVideoSize(
        const std::string& user_name,
        int width,
        int height);

    // ==================== 音频输出设备 ====================

    Result GetAudioSpeakers(
        std::vector<SpeakerDeviceInfo>& speakers);

    Result UpdateAudioSpeaker(
        const std::string& speaker_id);

    Result GetCurrentAudioSpeaker(
        std::string& speaker_id);

    // ==================== 会议事件观察者 ====================

    /**
     * @brief 注册会议事件观察者
     *
     * 重复注册同一个观察者时，内部实现应避免重复保存。
     */
    void AddObserver(
        const std::shared_ptr<IMeetingEventObserver>& observer);

    /**
     * @brief 移除会议事件观察者
     */
    void RemoveObserver(
        const std::shared_ptr<IMeetingEventObserver>& observer);

private:
    std::shared_ptr<VceEngineImpl> m_impl;

    static std::shared_ptr<VceEngine> m_instance;
    static std::mutex m_instance_mutex;
};

} // namespace VCE