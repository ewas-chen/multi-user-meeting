#pragma once

#include "ICaptureDataCallback.h"
#include "IMeetingEventObserver.h"
#include "IServiceDataCallback.h"
#include "ITransportDataCallback.h"
#include "VceGlobal.h"
#include "VceTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace VCE {

class CaptureManager;
class RenderManager;
class TransportManager;

namespace SERVICE {
class ServiceManager;
}

/**
 * @brief 远端用户媒体接收状态
 *
 * 状态在首次收到远端音频或视频帧时切换为true，
 * 在用户被移除或会议媒体被清理时切换为false。
 */
struct UserMediaState
{
    bool video_enable{false};
    bool audio_enable{false};
};

/**
 * @brief VideoConferenceEngine核心调度实现
 *
 * VceEngineImpl连接业务、采集、传输和渲染模块：
 *
 * 本地视频：
 * CaptureManager -> VceEngineImpl
 *                -> RenderManager本地预览
 *                -> TransportManager推流
 *
 * 本地音频：
 * CaptureManager -> VceEngineImpl
 *                -> TransportManager推流
 *
 * 远端音视频：
 * TransportManager -> VceEngineImpl
 *                  -> RenderManager
 *
 * Qt接入约束：
 * AddUser()、RemoveUser()、RenderUser()和UpdateUserVideoSize()
 * 涉及OpenGL资源，必须由持有对应OpenGL Context的Qt渲染线程调用。
 *
 * 会议业务线程只管理RTC订阅和用户媒体状态，不直接创建或销毁
 * OpenGL渲染资源。
 */
class VCE_LOCAL VceEngineImpl final
    : public ICaptureDataCallback,
      public ITransportDataCallback,
      public IServiceDataCallback,
      public std::enable_shared_from_this<VceEngineImpl>
{
public:
    VceEngineImpl();
    ~VceEngineImpl();

    VceEngineImpl(const VceEngineImpl&) = delete;
    VceEngineImpl& operator=(const VceEngineImpl&) = delete;
    VceEngineImpl(VceEngineImpl&&) = delete;
    VceEngineImpl& operator=(VceEngineImpl&&) = delete;

    // ==================== 引擎生命周期 ====================

    /**
     * @brief 初始化业务、渲染、传输和采集模块
     *
     * 必须在shared_ptr管理VceEngineImpl之后调用。
     * 任一模块初始化失败时会回滚已经初始化的模块。
     */
    Result InitSubModel(const EngineConfig& config);

    /**
     * @brief 反初始化全部子模块
     *
     * Qt客户端调用前必须先销毁所有VideoRenderWidget，
     * 确保OpenGL资源已经在正确Context中释放。
     */
    Result UninitSubModel();

    // ==================== 用户服务 ====================

    Result RegisterUser(const std::string& user_name,
                        const std::string& password);

    Result LoginUser(const std::string& user_name,
                     const std::string& password);

    // ==================== 会议管理 ====================

    /**
     * @brief 创建会议并配置当前会议的RTC媒体信息
     *
     * 不在此处创建本地OpenGL渲染资源。
     * 本地渲染用户由VideoRenderWidget::initializeGL()添加。
     */
    Result CreateMeeting(const CreateMeetingInfo& request,
                         CreateMeetingResponse& response);

    /**
     * @brief 加入会议并订阅响应中已有的远端参与者
     *
     * 这里只建立RTC订阅，不创建远端OpenGL渲染资源。
     * 远端渲染控件由Qt收到用户加入信息后创建。
     */
    Result JoinMeeting(const MeetingBriefInfo& request,
                       JoinMeetingResponse& response);

    Result LeaveMeeting(const MeetingBriefInfo& request);
    Result EndMeeting(const MeetingBriefInfo& request);

    Result GetMeetingList(const GetMeetingListRequest& request,
                          GetMeetingListResponse& response);

    // ==================== 采集设备管理 ====================

    Result GetCameraDevices(std::vector<CameraDeviceInfo>& devices);
    Result GetMicrophoneDevices(std::vector<MicDeviceInfo>& devices);

    Result GetCurrentCameraDeviceId(std::string& camera_device_id);
    Result GetCurrentMicrophoneDeviceId(std::string& microphone_device_id);

    Result UpdateCameraDevice(const std::string& camera_device_id);
    Result UpdateMicrophoneDevice(const std::string& microphone_device_id);

    Result OpenCamera();
    Result CloseCamera();

    Result OpenMic();
    Result CloseMic();

    // ==================== 渲染管理 ====================

    /**
     * @brief 创建指定用户的渲染资源
     *
     * 只能由VideoRenderWidget::initializeGL()在有效OpenGL Context中调用。
     * 本函数不负责建立RTC订阅。
     */
    Result AddUser(const std::string& user_name, bool is_local);

    /**
     * @brief 删除指定用户的渲染资源和其已有音频播放缓冲
     *
     * 只能由VideoRenderWidget在绑定对应OpenGL Context后调用。
     * 本函数不负责取消RTC订阅，也不发送用户离开通知。
     *
     * RenderEngine内部原有的AudioMixer用户缓冲清理保持不变。
     */
    Result RemoveUser(const std::string& user_name);

    /**
     * @brief 在当前OpenGL Context中渲染指定用户
     */
    Result RenderUser(const std::string& user_name);

    /**
     * @brief 更新指定用户的OpenGL显示区域
     */
    Result UpdateUserVideoSize(const std::string& user_name,
                               int width, int height);

    // ==================== 音频输出设备 ====================

    Result GetAudioSpeakers(std::vector<SpeakerDeviceInfo>& speakers);
    Result UpdateAudioSpeaker(const std::string& speaker_id);
    Result GetCurrentAudioSpeaker(std::string& speaker_id);

    // ==================== 会议事件观察者 ====================

    void AddObserver(
        const std::shared_ptr<IMeetingEventObserver>& observer);

    void RemoveObserver(
        const std::shared_ptr<IMeetingEventObserver>& observer);

protected:
    // ==================== 采集模块回调 ====================

    void OnCaptureVideoFrame(
        const std::shared_ptr<I420Frame>& frame) override;

    void OnCaptureAudioFrame(
        const std::shared_ptr<AudioFrame>& frame) override;

    // ==================== 传输模块回调 ====================

    void OnTransportVideoFrame(
        const std::string& user_id,
        CaptureType capture_type,
        const std::shared_ptr<I420Frame>& frame) override;

    void OnTransportAudioFrame(
        const std::string& user_id,
        const std::shared_ptr<AudioFrame>& frame) override;

    void OnTransportConnectionStateChanged(
        TransportState state) override;

    // ==================== 业务服务事件回调 ====================

    /**
     * @brief 服务端通知新用户加入
     *
     * 建立远端RTC订阅后通知Qt创建VideoRenderWidget。
     * 该回调可能运行在gRPC线程中，不能直接操作Qt控件或OpenGL。
     */
    void OnUserJoined(const std::string& user_name) override;

    /**
     * @brief 服务端通知用户离开
     *
     * 取消RTC订阅并通知Qt删除对应VideoRenderWidget。
     * OpenGL资源最终由Qt线程释放。
     */
    void OnUserLeft(const std::string& user_name) override;

    /**
     * @brief 服务端通知当前会议已经结束
     */
    void OnMeetingEnded() override;

private:
    /**
     * @brief 将会议及媒体服务器信息设置给TransportManager
     */
    Result SetRoomInfo(const std::string& meeting_id,
                       const std::string& local_user_name,
                       const MediaServerInfo& media_server);

    /**
     * @brief 为远端用户建立RTC音视频订阅
     *
     * 不创建OpenGL资源。如果订阅成功，同时创建该用户的
     * 媒体状态记录。
     */
    Result SubscribeRemoteUserMedia(const std::string& user_name);

    /**
     * @brief 取消远端用户的RTC音视频订阅并清理媒体状态
     *
     * 不删除OpenGL资源。Qt收到用户离开通知后负责删除
     * VideoRenderWidget并调用RemoveUser()。
     */
    void UnsubscribeRemoteUserMedia(const std::string& user_name);

    /**
     * @brief 停止当前会议的采集、发布和远端订阅
     *
     * 不发送LeaveMeeting或EndMeeting业务请求，
     * 也不直接释放OpenGL资源。
     *
     * Qt客户端应先销毁所有VideoRenderWidget，再调用
     * LeaveMeeting()、EndMeeting()或UninitSubModel()。
     */
    void CleanupMeetingMedia();

    // 更新远端用户首次收到视频帧的状态
    void MarkUserVideoReceived(const std::string& user_name);

    // 更新远端用户首次收到音频帧的状态
    void MarkUserAudioReceived(const std::string& user_name);

    // 删除远端媒体状态，并在必要时通知媒体关闭
    void RemoveUserMediaState(const std::string& user_name);

    /**
     * @brief 获取当前仍然有效的观察者快照
     *
     * 调用外部观察者时不持有m_observers_mutex，
     * 避免观察者回调再次进入VceEngine产生死锁。
     */
    std::vector<std::shared_ptr<IMeetingEventObserver>>
    GetObserverSnapshot();

    void NotifyUsersJoined(
        const std::vector<std::string>& user_names);

    void NotifyUsersLeft(
        const std::vector<std::string>& user_names);

    void NotifyMeetingEnded();

    void NotifyUserVideoEnable(
        const std::string& user_name,
        bool enable);

    void NotifyUserAudioEnable(
        const std::string& user_name,
        bool enable);

    void NotifyTransportStateChanged(TransportState state);

private:
    /*
     * Manager使用shared_ptr，回调或公开接口调用时可以先取得快照。
     * 这样反初始化清空成员后，正在执行的调用仍不会访问悬空对象。
     */
    std::shared_ptr<SERVICE::ServiceManager> m_service_manager;
    std::shared_ptr<CaptureManager> m_capture_manager;
    std::shared_ptr<RenderManager> m_render_manager;
    std::shared_ptr<TransportManager> m_transport_manager;

    /*
     * 保护Manager指针、当前会议信息和本地媒体开关状态。
     * 不应在持有此锁时调用RPC、观察者或可能阻塞的模块接口。
     */
    mutable std::mutex m_state_mutex;

    std::string m_current_meeting_id;
    std::string m_local_user_name;

    bool m_camera_open{false};
    bool m_microphone_open{false};

    std::atomic<bool> m_submodel_initialized{false};

    /*
     * 反初始化或离开会议时设置为false，
     * 使已经到达的采集和传输回调停止向下游转发。
     */
    std::atomic<bool> m_accept_media_callbacks{false};

    std::vector<std::weak_ptr<IMeetingEventObserver>> m_observers;
    mutable std::mutex m_observers_mutex;

    /*
     * 只记录远端用户是否已经收到音频或视频，
     * 不持有Qt控件或OpenGL资源。
     */
    std::unordered_map<std::string, UserMediaState> m_user_media_states;
    mutable std::mutex m_media_state_mutex;
};

} // namespace VCE