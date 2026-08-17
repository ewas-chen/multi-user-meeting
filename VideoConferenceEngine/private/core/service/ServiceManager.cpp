#include "ServiceManager.h"

#include "UserService.h"
#include "utils/logManager.h"

#include <exception>
#include <utility>

namespace VCE::SERVICE {

ServiceManager::~ServiceManager() {
    Uninitialize();
}

Result ServiceManager::Initialize(
    const ServiceConfig& config) {
    if (config.server_address.empty()) {
        LOG_ERROR(
            "ServiceManager initialization failed: "
            "server address is empty");

        return kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_initialized) {
        return kRet_SUCCESS;
    }

    try {
        /*
         * UserService和MeetingService访问同一个业务服务器，
         * 使用相同的RPC超时时间。
         */
        auto user_service =
            std::make_shared<UserService>(
                config.server_address,
                config.request_timeout);

        auto meeting_service =
            std::make_shared<MeetingService>(config);

        m_user_service = std::move(user_service);
        m_meeting_service = std::move(meeting_service);
        m_initialized = true;
    } catch (const std::exception& exception) {
        m_user_service.reset();
        m_meeting_service.reset();
        m_initialized = false;

        LOG_ERROR(
            "ServiceManager initialization failed: {}",
            exception.what());

        return kRet_Invalid_Status;
    } catch (...) {
        m_user_service.reset();
        m_meeting_service.reset();
        m_initialized = false;

        LOG_ERROR(
            "ServiceManager initialization failed: "
            "unknown exception");

        return kRet_Invalid_Status;
    }

    LOG_INFO(
        "ServiceManager initialized: server={}",
        config.server_address);

    return kRet_SUCCESS;
}

void ServiceManager::Uninitialize() {
    std::shared_ptr<UserService> user_service;
    std::shared_ptr<MeetingService> meeting_service;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_initialized &&
            !m_user_service &&
            !m_meeting_service) {
            return;
        }

        /*
         * 先从Manager中移出服务对象。
         * 已经取得shared_ptr快照的RPC可以安全执行完成，
         * 新请求则会因为无法取得服务对象而失败。
         */
        user_service = std::move(m_user_service);
        meeting_service = std::move(m_meeting_service);
        m_initialized = false;
    }

    meeting_service.reset();
    user_service.reset();

    LOG_INFO("ServiceManager uninitialized");
}

bool ServiceManager::IsInitialized() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_initialized;
}

Result ServiceManager::RegisterUser(
    const std::string& user_name,
    const std::string& password) {
    const auto service = GetUserService();

    if (!service) {
        LOG_ERROR(
            "Register user failed: "
            "ServiceManager is not initialized");

        return kRet_Invalid_Status;
    }

    /*
     * 参数检查由UserService统一完成，
     * ServiceManager只负责转发，避免重复检查。
     */
    return service->RegisterUser(user_name, password);
}

Result ServiceManager::LoginUser(
    const std::string& user_name,
    const std::string& password) {
    const auto service = GetUserService();

    if (!service) {
        LOG_ERROR(
            "Login user failed: "
            "ServiceManager is not initialized");

        return kRet_Invalid_Status;
    }

    return service->LoginUser(user_name, password);
}

Result ServiceManager::CreateMeeting(
    const CreateMeetingInfo& request,
    CreateMeetingResponse& response) {
    const auto service = GetMeetingService();

    if (!service) {
        LOG_ERROR(
            "Create meeting failed: "
            "ServiceManager is not initialized");

        return kRet_Invalid_Status;
    }

    return service->CreateMeeting(request, response);
}

Result ServiceManager::JoinMeeting(
    const MeetingBriefInfo& request,
    JoinMeetingResponse& response) {
    const auto service = GetMeetingService();

    if (!service) {
        LOG_ERROR(
            "Join meeting failed: "
            "ServiceManager is not initialized");

        return kRet_Invalid_Status;
    }

    return service->JoinMeeting(request, response);
}

Result ServiceManager::LeaveMeeting(
    const MeetingBriefInfo& request) {
    const auto service = GetMeetingService();

    if (!service) {
        LOG_ERROR(
            "Leave meeting failed: "
            "ServiceManager is not initialized");

        return kRet_Invalid_Status;
    }

    return service->LeaveMeeting(request);
}

Result ServiceManager::EndMeeting(
    const MeetingBriefInfo& request) {
    const auto service = GetMeetingService();

    if (!service) {
        LOG_ERROR(
            "End meeting failed: "
            "ServiceManager is not initialized");

        return kRet_Invalid_Status;
    }

    return service->EndMeeting(request);
}

Result ServiceManager::GetMeetingList(
    const GetMeetingListRequest& request,
    GetMeetingListResponse& response) {
    const auto service = GetMeetingService();

    if (!service) {
        LOG_ERROR(
            "Get meeting list failed: "
            "ServiceManager is not initialized");

        return kRet_Invalid_Status;
    }

    return service->GetMeetingList(request, response);
}

std::shared_ptr<UserService>
ServiceManager::GetUserService() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized) {
        return nullptr;
    }

    return m_user_service;
}

std::shared_ptr<MeetingService>
ServiceManager::GetMeetingService() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized) {
        return nullptr;
    }

    return m_meeting_service;
}

void ServiceManager::SetServiceDataCallback(
    std::shared_ptr<::VCE::IServiceDataCallback> callback) {
    /*
     * 使用shared_ptr快照保证转发期间MeetingService对象有效，
     * 不持有ServiceManager状态锁执行下层操作。
     */
    const auto service = GetMeetingService();

    if (!service) {
        if (callback) {
            LOG_WARN(
                "Set service data callback ignored: "
                "ServiceManager is not initialized");
        }

        return;
    }

    service->SetEventCallback(std::move(callback));
}

} // namespace VCE::SERVICE