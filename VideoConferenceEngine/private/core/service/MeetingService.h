#pragma once

#include "IServiceDataCallback.h"
#include "VceTypes.h"
#include "meeting_service.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace VCE::SERVICE {

/**
 * @brief 会议业务gRPC客户端
 *
 * 负责创建、加入、离开和结束会议，并通过SubscribeMeetingEvents
 * 持续接收用户加入、用户离开和会议结束事件。
 *
 * protobuf类型只在Service模块内部使用，对外统一转换为VceTypes
 * 和IServiceDataCallback。
 */
class MeetingService final {
public:
    explicit MeetingService(const ServiceConfig& config);
    ~MeetingService();

    MeetingService(const MeetingService&) = delete;
    MeetingService& operator=(const MeetingService&) = delete;
    MeetingService(MeetingService&&) = delete;
    MeetingService& operator=(MeetingService&&) = delete;

    /**
     * @brief 设置会议事件接收对象
     *
     * 使用weak_ptr保存回调，避免MeetingService与VceEngineImpl形成循环引用。
     * 传入nullptr可以停止向上层分发事件，但不会直接改变当前会议状态。
     */
    void SetEventCallback(std::shared_ptr<::VCE::IServiceDataCallback> callback);

    Result CreateMeeting(const CreateMeetingInfo& request, CreateMeetingResponse& response);
    Result JoinMeeting(const MeetingBriefInfo& request, JoinMeetingResponse& response);
    Result LeaveMeeting(const MeetingBriefInfo& request);
    Result EndMeeting(const MeetingBriefInfo& request);

    Result GetMeetingList(const GetMeetingListRequest& request,
                          GetMeetingListResponse& response);

private:
    /**
     * @brief 启动当前会议的服务端事件流
     *
     * 创建或加入会议成功后调用。事件流不设置普通RPC超时，
     * 其生命周期由StopMeetingEventSubscription通过TryCancel终止。
     */
    Result StartMeetingEventSubscription(const std::string& user_name,
                                         const std::string& meeting_id);

    /**
     * @brief 停止事件流并等待读取线程退出
     *
     * 可以重复调用。离开会议、结束会议以及析构时都会调用。
     */
    void StopMeetingEventSubscription();

    /**
     * @brief 在已经持有m_event_lifecycle_mutex时停止事件流
     *
     * 单独提供该函数，使启动新订阅前可以安全清理旧订阅，
     * 避免重复加锁或覆盖仍然joinable的线程。
     */
    void StopMeetingEventSubscriptionLocked();

    /**
     * @brief 会议事件读取线程入口
     *
     * request和context由线程持有，保证阻塞读取期间对象有效。
     */
    void MeetingEventLoop(
        ::meeting_service::SubscribeMeetingEventsRequest request,
        std::shared_ptr<grpc::ClientContext> context);

    /**
     * @brief 校验并分发一条会议事件
     *
     * 返回false表示会议已经结束，读取线程应退出；
     * 返回true表示继续读取后续事件。
     */
    bool HandleMeetingEvent(const ::meeting_service::MeetingEvent& event,
                            const std::string& expected_meeting_id);

    /**
     * @brief 根据用户名和配置的客户端IPv4构造protobuf用户信息
     */
    std::optional<::meeting_service::UserInfo>
    CreateUserInfo(const std::string& user_name) const;

    /**
     * @brief 将服务端返回的IPv4整数转换为完整媒体服务器配置
     */
    bool BuildMediaServerInfo(std::uint32_t push_server_ip,
                              std::uint32_t pull_server_ip,
                              MediaServerInfo& media_server) const;

private:
    ServiceConfig m_config;

    std::unique_ptr<::meeting_service::MeetingService::Stub> m_stub;

    // 回调单独加锁，事件线程分发期间不持有订阅生命周期锁。
    mutable std::mutex m_event_callback_mutex;
    std::weak_ptr<::VCE::IServiceDataCallback> m_event_callback;

    /*
     * 保护事件线程和ClientContext的创建、取消及回收。
     * 不允许同时存在多个会议事件订阅。
     */
    std::mutex m_event_lifecycle_mutex;
    std::shared_ptr<grpc::ClientContext> m_event_context;
    std::thread m_event_thread;
    std::atomic<bool> m_event_running{false};
};

} // namespace VCE::SERVICE