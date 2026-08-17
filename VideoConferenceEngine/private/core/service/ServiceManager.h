#pragma once

#include "IServiceDataCallback.h"
#include "MeetingService.h"
#include "VceTypes.h"

#include <memory>
#include <mutex>
#include <string>

namespace VCE::SERVICE {

class UserService;

/**
 * @brief 业务服务统一管理器
 *
 * ServiceManager负责：
 * 1. 统一创建和释放UserService、MeetingService；
 * 2. 向VceEngineImpl提供用户和会议业务接口；
 * 3. 将会议事件回调转交给MeetingService；
 * 4. 隐藏protobuf和gRPC调用细节。
 *
 * ServiceManager不负责采集、渲染和RTC传输。
 */
class ServiceManager final {
public:
    ServiceManager() = default;
    ~ServiceManager();

    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;
    ServiceManager(ServiceManager&&) = delete;
    ServiceManager& operator=(ServiceManager&&) = delete;

    /**
     * @brief 初始化业务服务
     *
     * UserService和MeetingService使用相同的gRPC服务地址和超时时间。
     * 重复调用时，如果已经初始化则直接返回成功。
     */
    Result Initialize(const ServiceConfig& config);

    /**
     * @brief 释放业务服务
     *
     * 已经取得shared_ptr快照的RPC可以安全执行完成；
     * 新请求会因无法取得服务对象而失败。
     *
     * MeetingService析构时会取消会议事件流并回收读取线程。
     */
    void Uninitialize();

    bool IsInitialized() const;

    /**
     * @brief 设置会议事件回调
     *
     * 回调由MeetingService以weak_ptr保存，不会形成循环引用。
     * 传入nullptr用于停止向VceEngineImpl分发会议事件。
     */
    void SetServiceDataCallback(
        std::shared_ptr<::VCE::IServiceDataCallback> callback);

    // ==================== 用户服务 ====================

    Result RegisterUser(const std::string& user_name,
                        const std::string& password);

    Result LoginUser(const std::string& user_name,
                     const std::string& password);

    // ==================== 会议服务 ====================

    Result CreateMeeting(const CreateMeetingInfo& request,
                         CreateMeetingResponse& response);

    Result JoinMeeting(const MeetingBriefInfo& request,
                       JoinMeetingResponse& response);

    Result LeaveMeeting(const MeetingBriefInfo& request);
    Result EndMeeting(const MeetingBriefInfo& request);

    Result GetMeetingList(const GetMeetingListRequest& request,
                          GetMeetingListResponse& response);

private:
    /*
     * 调用RPC前取得服务对象的shared_ptr快照。
     * 获取快照后立即释放Manager互斥锁，避免持锁等待网络请求。
     */
    std::shared_ptr<UserService> GetUserService() const;
    std::shared_ptr<MeetingService> GetMeetingService() const;

private:
    mutable std::mutex m_state_mutex;

    std::shared_ptr<UserService> m_user_service;
    std::shared_ptr<MeetingService> m_meeting_service;

    bool m_initialized{false};
};

} // namespace VCE::SERVICE