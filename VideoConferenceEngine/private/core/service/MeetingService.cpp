#include "MeetingService.h"

#include "utils/logManager.h"

#include <arpa/inet.h>
#include <grpcpp/grpcpp.h>

#include <exception>
#include <utility>

namespace {

std::string NetworkIPv4ToString(std::uint32_t network_ip) {
    if (network_ip == 0) {
        return {};
    }

    in_addr address{};
    address.s_addr = network_ip;

    char buffer[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address, buffer, sizeof(buffer))) {
        return {};
    }

    return buffer;
}

} // namespace

namespace VCE::SERVICE {

MeetingService::MeetingService(const ServiceConfig& config)
    : m_config(config) {
    if (m_config.server_address.empty()) {
        LOG_ERROR("Failed to create MeetingService: server address is empty");
        return;
    }

    if (m_config.request_timeout.count() <= 0) {
        m_config.request_timeout = std::chrono::milliseconds{5000};
    }

    /*
     * 与当前服务端保持一致，使用非TLS gRPC连接。
     * 实际连接通常在第一次RPC调用时建立。
     */
    auto channel = grpc::CreateChannel(
        m_config.server_address,
        grpc::InsecureChannelCredentials());

    if (!channel) {
        LOG_ERROR("Failed to create MeetingService gRPC channel");
        return;
    }

    m_stub = ::meeting_service::MeetingService::NewStub(channel);
    if (!m_stub) {
        LOG_ERROR("Failed to create MeetingService gRPC stub");
    }
}

MeetingService::~MeetingService() {
    /*
     * 先清除回调，阻止析构期间继续向上层分发事件，
     * 再取消阻塞的gRPC读取并回收线程。
     */
    SetEventCallback(nullptr);
    StopMeetingEventSubscription();
}

void MeetingService::SetEventCallback(
    std::shared_ptr<::VCE::IServiceDataCallback> callback) {
    std::lock_guard<std::mutex> lock(m_event_callback_mutex);
    m_event_callback = std::move(callback);
}

Result MeetingService::CreateMeeting(
    const CreateMeetingInfo& request_info,
    CreateMeetingResponse& output) {
    output = {};

    if (request_info.user_name.empty() || request_info.title.empty()) {
        LOG_ERROR("Create meeting failed: user name or meeting title is empty");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR("Create meeting failed: MeetingService stub is unavailable");
        return kRet_Invalid_Status;
    }

    if (m_config.media_http_scheme.empty() || m_config.media_http_port == 0 ||
        m_config.media_rtc_port == 0 || m_config.whip_path.empty() ||
        m_config.whep_path.empty() || m_config.app_name.empty()) {
        LOG_ERROR("Create meeting failed: media server configuration is invalid");
        return kRet_InvalidParam;
    }

    const auto user = CreateUserInfo(request_info.user_name);
    if (!user) {
        LOG_ERROR("Create meeting failed: invalid client IPv4 address: {}",
                  m_config.client_ip);
        return kRet_NoClientIp;
    }

    ::meeting_service::CreateMeetingRequest request;
    *request.mutable_user() = *user;
    request.set_meetingtitle(request_info.title);
    request.set_meetingdescription(request_info.description);
    request.set_starttime(request_info.start_time);

    ::meeting_service::CreateMeetingResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         m_config.request_timeout);

    const grpc::Status status = m_stub->CreateMeeting(
        &context, request, &response);

    if (!status.ok()) {
        LOG_ERROR(
            "Create meeting RPC failed: title={}, grpc_code={}, message={}",
            request_info.title,
            static_cast<int>(status.error_code()),
            status.error_message());
        return kRet_Invalid_Status;
    }

    if (response.errorcode() != 0) {
        LOG_ERROR(
            "Create meeting rejected: title={}, error_code={}, message={}",
            request_info.title,
            response.errorcode(),
            response.errormessage());
        return kRet_Error_Response;
    }

    MediaServerInfo media_server;
    if (!BuildMediaServerInfo(response.pushmeetingserverip(),
                              response.pullmeetingserverip(),
                              media_server)) {
        LOG_ERROR(
            "Create meeting succeeded but media server information is invalid");
        return kRet_Error_Response;
    }

    output.meeting_id = response.meetingid();
    output.media_server = std::move(media_server);

    /*
     * 创建成功后立即订阅会议事件。
     * 后续用户加入通知才能驱动远端媒体订阅。
     */
    const Result subscription_result =
        StartMeetingEventSubscription(request_info.user_name,
                                      output.meeting_id);

    if (subscription_result != kRet_SUCCESS) {
        LOG_ERROR(
            "Meeting created but event subscription failed: meeting_id={}",
            output.meeting_id);
        return subscription_result;
    }

    LOG_INFO("Meeting created successfully: meeting_id={}, title={}",
             output.meeting_id,
             request_info.title);

    return kRet_SUCCESS;
}

Result MeetingService::JoinMeeting(
    const MeetingBriefInfo& request_info,
    JoinMeetingResponse& output) {
    output = {};

    if (request_info.user_name.empty() || request_info.meeting_id.empty()) {
        LOG_ERROR("Join meeting failed: user name or meeting ID is empty");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR("Join meeting failed: MeetingService stub is unavailable");
        return kRet_Invalid_Status;
    }

    if (m_config.media_http_scheme.empty() || m_config.media_http_port == 0 ||
        m_config.media_rtc_port == 0 || m_config.whip_path.empty() ||
        m_config.whep_path.empty() || m_config.app_name.empty()) {
        LOG_ERROR("Join meeting failed: media server configuration is invalid");
        return kRet_InvalidParam;
    }

    const auto user = CreateUserInfo(request_info.user_name);
    if (!user) {
        LOG_ERROR("Join meeting failed: invalid client IPv4 address: {}",
                  m_config.client_ip);
        return kRet_NoClientIp;
    }

    ::meeting_service::JoinMeetingRequest request;
    *request.mutable_user() = *user;
    request.set_meetingid(request_info.meeting_id);

    ::meeting_service::JoinMeetingResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         m_config.request_timeout);

    const grpc::Status status = m_stub->JoinMeeting(
        &context, request, &response);

    if (!status.ok()) {
        LOG_ERROR(
            "Join meeting RPC failed: meeting_id={}, grpc_code={}, message={}",
            request_info.meeting_id,
            static_cast<int>(status.error_code()),
            status.error_message());
        return kRet_Invalid_Status;
    }

    if (response.errorcode() != 0) {
        LOG_ERROR(
            "Join meeting rejected: meeting_id={}, error_code={}, message={}",
            request_info.meeting_id,
            response.errorcode(),
            response.errormessage());
        return kRet_Error_Response;
    }

    MediaServerInfo media_server;
    if (!BuildMediaServerInfo(response.pushmeetingserverip(),
                              response.pullmeetingserverip(),
                              media_server)) {
        LOG_ERROR(
            "Join meeting succeeded but media server information is invalid");
        return kRet_Error_Response;
    }

    output.meeting_title = response.meetingtitle();
    output.meeting_description = response.meetingdescription();
    output.media_server = std::move(media_server);
    output.participants.reserve(
        static_cast<std::size_t>(response.participants_size()));

    for (const auto& participant : response.participants()) {
        UserInfo participant_info;
        participant_info.user_name = participant.username();
        participant_info.client_ip =
            NetworkIPv4ToString(participant.clientip());
        output.participants.emplace_back(std::move(participant_info));
    }

    /*
     * JoinMeeting响应提供当前参与者列表，
     * 事件流负责接收此后发生的增量变化。
     */
    const Result subscription_result =
        StartMeetingEventSubscription(request_info.user_name,
                                      request_info.meeting_id);

    if (subscription_result != kRet_SUCCESS) {
        LOG_ERROR(
            "Joined meeting but event subscription failed: meeting_id={}",
            request_info.meeting_id);
        return subscription_result;
    }

    LOG_INFO("Joined meeting successfully: meeting_id={}, participants={}",
             request_info.meeting_id,
             output.participants.size());

    return kRet_SUCCESS;
}

Result MeetingService::LeaveMeeting(
    const MeetingBriefInfo& request_info) {
    if (request_info.user_name.empty() || request_info.meeting_id.empty()) {
        LOG_ERROR("Leave meeting failed: user name or meeting ID is empty");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR("Leave meeting failed: MeetingService stub is unavailable");
        return kRet_Invalid_Status;
    }

    const auto user = CreateUserInfo(request_info.user_name);
    if (!user) {
        LOG_ERROR("Leave meeting failed: invalid client IPv4 address: {}",
                  m_config.client_ip);
        return kRet_NoClientIp;
    }

    ::meeting_service::LeaveMeetingRequest request;
    *request.mutable_user() = *user;
    request.set_meetingid(request_info.meeting_id);

    ::meeting_service::LeaveMeetingResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         m_config.request_timeout);

    const grpc::Status status = m_stub->LeaveMeeting(
        &context, request, &response);

    if (!status.ok()) {
        LOG_ERROR(
            "Leave meeting RPC failed: meeting_id={}, grpc_code={}, message={}",
            request_info.meeting_id,
            static_cast<int>(status.error_code()),
            status.error_message());
        return kRet_Invalid_Status;
    }

    if (response.errorcode() != 0) {
        LOG_ERROR(
            "Leave meeting rejected: meeting_id={}, error_code={}, message={}",
            request_info.meeting_id,
            response.errorcode(),
            response.errormessage());
        return kRet_Error_Response;
    }

    /*
     * 只有服务端确认离开后才停止订阅。
     * RPC失败时仍保留原事件流，避免本地状态与服务端脱节。
     */
    StopMeetingEventSubscription();

    LOG_INFO("Left meeting successfully: meeting_id={}, user={}",
             request_info.meeting_id,
             request_info.user_name);

    return kRet_SUCCESS;
}

Result MeetingService::EndMeeting(
    const MeetingBriefInfo& request_info) {
    if (request_info.user_name.empty() || request_info.meeting_id.empty()) {
        LOG_ERROR("End meeting failed: user name or meeting ID is empty");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR("End meeting failed: MeetingService stub is unavailable");
        return kRet_Invalid_Status;
    }

    const auto user = CreateUserInfo(request_info.user_name);
    if (!user) {
        LOG_ERROR("End meeting failed: invalid client IPv4 address: {}",
                  m_config.client_ip);
        return kRet_NoClientIp;
    }

    ::meeting_service::EndMeetingRequest request;
    *request.mutable_user() = *user;
    request.set_meetingid(request_info.meeting_id);

    ::meeting_service::EndMeetingResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         m_config.request_timeout);

    const grpc::Status status = m_stub->EndMeeting(
        &context, request, &response);

    if (!status.ok()) {
        LOG_ERROR(
            "End meeting RPC failed: meeting_id={}, grpc_code={}, message={}",
            request_info.meeting_id,
            static_cast<int>(status.error_code()),
            status.error_message());
        return kRet_Invalid_Status;
    }

    if (response.errorcode() != 0) {
        LOG_ERROR(
            "End meeting rejected: meeting_id={}, error_code={}, message={}",
            request_info.meeting_id,
            response.errorcode(),
            response.errormessage());
        return kRet_Error_Response;
    }

    StopMeetingEventSubscription();

    LOG_INFO("Meeting ended successfully: meeting_id={}",
             request_info.meeting_id);

    return kRet_SUCCESS;
}

Result MeetingService::GetMeetingList(
    const GetMeetingListRequest& request_info,
    GetMeetingListResponse& output) {
    output = {};

    if (request_info.user_name.empty() ||
        request_info.page_size <= 0 ||
        request_info.page_number <= 0) {
        LOG_ERROR("Get meeting list failed: invalid request parameters");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR(
            "Get meeting list failed: MeetingService stub is unavailable");
        return kRet_Invalid_Status;
    }

    const auto user = CreateUserInfo(request_info.user_name);
    if (!user) {
        LOG_ERROR(
            "Get meeting list failed: invalid client IPv4 address: {}",
            m_config.client_ip);
        return kRet_NoClientIp;
    }

    ::meeting_service::GetMeetingListRequest request;
    *request.mutable_user() = *user;
    request.set_pagesize(request_info.page_size);
    request.set_pagenumber(request_info.page_number);

    ::meeting_service::GetMeetingListResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         m_config.request_timeout);

    const grpc::Status status = m_stub->GetMeetingList(
        &context, request, &response);

    if (!status.ok()) {
        if (status.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
            LOG_ERROR(
                "GetMeetingList is declared in proto but is not "
                "implemented by the current server");
        } else {
            LOG_ERROR(
                "Get meeting list RPC failed: grpc_code={}, message={}",
                static_cast<int>(status.error_code()),
                status.error_message());
        }

        return kRet_Invalid_Status;
    }

    if (response.errorcode() != 0) {
        LOG_ERROR(
            "Get meeting list rejected: error_code={}, message={}",
            response.errorcode(),
            response.errormessage());
        return kRet_Error_Response;
    }

    output.meetings.reserve(
        static_cast<std::size_t>(response.meetings_size()));

    for (const auto& meeting : response.meetings()) {
        MeetingInfo meeting_info;
        meeting_info.meeting_id = meeting.meetingid();
        meeting_info.title = meeting.meetingtitle();
        meeting_info.description = meeting.meetingdescription();
        meeting_info.start_time = meeting.starttime();
        meeting_info.duration = meeting.duration();
        meeting_info.creator_user_name = meeting.creator().username();
        meeting_info.creator_client_ip = meeting.creator().clientip();
        meeting_info.participant_count = meeting.participantcount();
        meeting_info.is_active = meeting.isactive();

        output.meetings.emplace_back(std::move(meeting_info));
    }

    output.total_count = response.totalcount();
    output.total_pages = response.totalpages();

    LOG_INFO("Meeting list received: count={}, total_count={}, total_pages={}",
             output.meetings.size(),
             output.total_count,
             output.total_pages);

    return kRet_SUCCESS;
}

Result MeetingService::StartMeetingEventSubscription(
    const std::string& user_name,
    const std::string& meeting_id) {
    if (user_name.empty() || meeting_id.empty()) {
        LOG_ERROR(
            "Start meeting event subscription failed: invalid parameters");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR(
            "Start meeting event subscription failed: stub is unavailable");
        return kRet_Invalid_Status;
    }

    const auto user = CreateUserInfo(user_name);
    if (!user) {
        LOG_ERROR(
            "Start meeting event subscription failed: invalid client IPv4: {}",
            m_config.client_ip);
        return kRet_NoClientIp;
    }

    ::meeting_service::SubscribeMeetingEventsRequest request;
    *request.mutable_user() = *user;
    request.set_meeting_id(meeting_id);

    std::lock_guard<std::mutex> lock(m_event_lifecycle_mutex);

    /*
     * 当前引擎一次只进入一个会议。
     * 启动新事件流前必须完整回收旧事件流。
     */
    StopMeetingEventSubscriptionLocked();

    if (m_event_thread.joinable()) {
        LOG_ERROR(
            "Start meeting event subscription failed: "
            "previous event thread cannot be joined");
        return kRet_Invalid_Status;
    }

    auto context = std::make_shared<grpc::ClientContext>();
    m_event_context = context;
    m_event_running.store(true, std::memory_order_release);

    try {
        m_event_thread = std::thread(
            &MeetingService::MeetingEventLoop,
            this,
            std::move(request),
            std::move(context));
    } catch (const std::exception& exception) {
        m_event_running.store(false, std::memory_order_release);
        m_event_context.reset();

        LOG_ERROR(
            "Start meeting event subscription failed: meeting_id={}, error={}",
            meeting_id,
            exception.what());
        return kRet_Invalid_Status;
    } catch (...) {
        m_event_running.store(false, std::memory_order_release);
        m_event_context.reset();

        LOG_ERROR(
            "Start meeting event subscription failed: "
            "meeting_id={}, unknown error",
            meeting_id);
        return kRet_Invalid_Status;
    }

    LOG_INFO("Meeting event subscription started: meeting_id={}, user={}",
             meeting_id,
             user_name);

    return kRet_SUCCESS;
}

void MeetingService::StopMeetingEventSubscription() {
    std::lock_guard<std::mutex> lock(m_event_lifecycle_mutex);
    StopMeetingEventSubscriptionLocked();
}

void MeetingService::StopMeetingEventSubscriptionLocked() {
    m_event_running.store(false, std::memory_order_release);

    /*
     * ClientReader::Read可能长期阻塞。
     * TryCancel会使Read和Finish退出，再由当前线程执行join。
     */
    if (m_event_context) {
        m_event_context->TryCancel();
    }

    if (m_event_thread.joinable()) {
        /*
         * 正常设计下停止操作由业务线程调用，不会发生在线程自身。
         * 保留检查可以避免错误调用直接触发自连接异常。
         */
        if (m_event_thread.get_id() == std::this_thread::get_id()) {
            LOG_WARN(
                "Meeting event thread requested to stop itself; "
                "thread join is deferred");
            return;
        }

        m_event_thread.join();
    }

    m_event_context.reset();
}

void MeetingService::MeetingEventLoop(
    ::meeting_service::SubscribeMeetingEventsRequest request,
    std::shared_ptr<grpc::ClientContext> context) {
    const std::string meeting_id = request.meeting_id();

    /*
     * 会议事件流持续到离会、会议结束或对象析构，
     * 因此这里不使用普通RPC的request_timeout。
     */
    auto reader = m_stub->SubscribeMeetingEvents(context.get(), request);

    if (!reader) {
        m_event_running.store(false, std::memory_order_release);
        LOG_ERROR(
            "SubscribeMeetingEvents failed to create reader: meeting_id={}",
            meeting_id);
        return;
    }

    ::meeting_service::MeetingEvent event;

    while (m_event_running.load(std::memory_order_acquire) &&
           reader->Read(&event)) {
        if (!m_event_running.load(std::memory_order_acquire)) {
            break;
        }

        if (!HandleMeetingEvent(event, meeting_id)) {
            /*
             * 收到会议结束事件后不再等待服务端继续推送。
             * 主动取消可以保证Finish不会无限等待。
             */
            m_event_running.store(false, std::memory_order_release);
            context->TryCancel();
            break;
        }

        event.Clear();
    }

    const grpc::Status status = reader->Finish();
    const bool stop_requested =
        !m_event_running.exchange(false, std::memory_order_acq_rel);

    if (!status.ok() && !stop_requested) {
        LOG_ERROR(
            "Meeting event stream ended unexpectedly: "
            "meeting_id={}, grpc_code={}, message={}",
            meeting_id,
            static_cast<int>(status.error_code()),
            status.error_message());
        return;
    }

    if (status.ok() && !stop_requested) {
        LOG_WARN(
            "Meeting event stream closed by server: meeting_id={}",
            meeting_id);
        return;
    }

    LOG_INFO("Meeting event subscription stopped: meeting_id={}",
             meeting_id);
}

bool MeetingService::HandleMeetingEvent(
    const ::meeting_service::MeetingEvent& event,
    const std::string& expected_meeting_id) {
    /*
     * 兼容未填写meeting_id的旧服务端事件。
     * 如果服务端明确填写了其他会议ID，则忽略该事件。
     */
    if (!event.meeting_id().empty() &&
        event.meeting_id() != expected_meeting_id) {
        LOG_WARN(
            "Ignored meeting event from another meeting: "
            "expected={}, actual={}",
            expected_meeting_id,
            event.meeting_id());
        return true;
    }

    std::shared_ptr<::VCE::IServiceDataCallback> callback;
    {
        std::lock_guard<std::mutex> lock(m_event_callback_mutex);
        callback = m_event_callback.lock();
    }

    switch (event.event_type()) {
    case ::meeting_service::EVENT_USER_JOINED: {
        const std::string user_name =
            event.trigger_user().username();

        if (user_name.empty()) {
            LOG_WARN(
                "Ignored user joined event without user name: meeting_id={}",
                expected_meeting_id);
            return true;
        }

        LOG_INFO("Meeting event: user joined, meeting_id={}, user={}",
                 expected_meeting_id,
                 user_name);

        if (callback) {
            try {
                callback->OnUserJoined(user_name);
            } catch (const std::exception& exception) {
                LOG_ERROR(
                    "OnUserJoined callback failed: user={}, error={}",
                    user_name,
                    exception.what());
            } catch (...) {
                LOG_ERROR(
                    "OnUserJoined callback failed: user={}, unknown error",
                    user_name);
            }
        }

        return true;
    }

    case ::meeting_service::EVENT_USER_LEFT: {
        const std::string user_name =
            event.trigger_user().username();

        if (user_name.empty()) {
            LOG_WARN(
                "Ignored user left event without user name: meeting_id={}",
                expected_meeting_id);
            return true;
        }

        LOG_INFO("Meeting event: user left, meeting_id={}, user={}",
                 expected_meeting_id,
                 user_name);

        if (callback) {
            try {
                callback->OnUserLeft(user_name);
            } catch (const std::exception& exception) {
                LOG_ERROR(
                    "OnUserLeft callback failed: user={}, error={}",
                    user_name,
                    exception.what());
            } catch (...) {
                LOG_ERROR(
                    "OnUserLeft callback failed: user={}, unknown error",
                    user_name);
            }
        }

        return true;
    }

    case ::meeting_service::EVENT_MEETING_ENDED:
        LOG_INFO("Meeting event: meeting ended, meeting_id={}",
                 expected_meeting_id);

        if (callback) {
            try {
                callback->OnMeetingEnded();
            } catch (const std::exception& exception) {
                LOG_ERROR(
                    "OnMeetingEnded callback failed: error={}",
                    exception.what());
            } catch (...) {
                LOG_ERROR(
                    "OnMeetingEnded callback failed: unknown error");
            }
        }

        return false;

    case ::meeting_service::EVENT_UNKNOWN:
    default:
        LOG_WARN(
            "Ignored unknown meeting event: meeting_id={}, event_type={}",
            expected_meeting_id,
            static_cast<int>(event.event_type()));
        return true;
    }
}

std::optional<::meeting_service::UserInfo>
MeetingService::CreateUserInfo(
    const std::string& user_name) const {
    if (user_name.empty() || m_config.client_ip.empty()) {
        return std::nullopt;
    }

    in_addr address{};
    if (inet_pton(AF_INET, m_config.client_ip.c_str(), &address) != 1 ||
        address.s_addr == 0) {
        return std::nullopt;
    }

    ::meeting_service::UserInfo user;
    user.set_username(user_name);

    /*
     * 服务端使用addr.s_addr保存网络字节序IPv4，
     * 因此这里不能调用ntohl()。
     */
    user.set_clientip(address.s_addr);
    return user;
}

bool MeetingService::BuildMediaServerInfo(
    std::uint32_t push_server_ip,
    std::uint32_t pull_server_ip,
    MediaServerInfo& media_server) const {
    const std::string push_ip =
        NetworkIPv4ToString(push_server_ip);
    const std::string pull_ip =
        NetworkIPv4ToString(pull_server_ip);

    if (push_ip.empty() || pull_ip.empty()) {
        return false;
    }

    /*
     * 当前TransportTargetRoomInfo只有一个rtc_external_address。
     * 当前服务端会将推流和拉流服务器设置为同一台机器。
     */
    if (push_ip != pull_ip) {
        LOG_ERROR(
            "Different push and pull media servers are not supported: "
            "push={}, pull={}",
            push_ip,
            pull_ip);
        return false;
    }

    std::string whip_path = m_config.whip_path;
    std::string whep_path = m_config.whep_path;

    if (whip_path.front() != '/') {
        whip_path.insert(whip_path.begin(), '/');
    }

    if (whep_path.front() != '/') {
        whep_path.insert(whep_path.begin(), '/');
    }

    const std::string http_port =
        std::to_string(m_config.media_http_port);

    media_server.push_server_url =
        m_config.media_http_scheme + "://" +
        push_ip + ":" + http_port + whip_path;

    media_server.pull_server_url =
        m_config.media_http_scheme + "://" +
        pull_ip + ":" + http_port + whep_path;

    media_server.app_name = m_config.app_name;
    media_server.rtc_external_address =
        push_ip + ":" + std::to_string(m_config.media_rtc_port);
    media_server.publish_secret = m_config.publish_secret;

    return true;
}

} // namespace VCE::SERVICE