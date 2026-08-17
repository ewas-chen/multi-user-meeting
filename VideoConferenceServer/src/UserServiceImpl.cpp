#include "UserServiceImpl.h"

#include <iostream>
#include <utility>

namespace VCE::TEST_SERVER {

UserServiceImpl::UserServiceImpl(
    std::shared_ptr<InMemoryMeetingStore> store)
    : m_store(std::move(store))
{
}

grpc::Status UserServiceImpl::Register(
    grpc::ServerContext* context,
    const ::user_service::RegisterRequest* request,
    ::user_service::RegisterResponse* response)
{
    (void)context;

    if (!request || !response) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "request or response is null");
    }

    if (!m_store) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store is unavailable");
    }

    const StoreResult result =
        m_store->RegisterUser(
            request->username(),
            request->password());

    response->set_error_code(
        RegisterErrorCode(result.error));

    response->set_error_message(result.message);

    if (result.IsSuccess()) {
        std::cout
            << "[TestServer] User registered: "
            << request->username()
            << std::endl;
    } else {
        std::cout
            << "[TestServer] Register rejected: user="
            << request->username()
            << ", code="
            << response->error_code()
            << ", message="
            << result.message
            << std::endl;
    }

    /*
     * 客户端通过error_code判断注册业务结果。
     * 只有协议或服务状态异常时才返回非OK的gRPC状态。
     */
    return grpc::Status::OK;
}

grpc::Status UserServiceImpl::Login(
    grpc::ServerContext* context,
    const ::user_service::LoginRequest* request,
    ::user_service::LoginResponse* response)
{
    (void)context;

    if (!request || !response) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "request or response is null");
    }

    if (!m_store) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store is unavailable");
    }

    const StoreResult result =
        m_store->LoginUser(
            request->username(),
            request->password());

    response->set_error_code(
        LoginErrorCode(result.error));

    response->set_error_message(result.message);

    if (result.IsSuccess()) {
        std::cout
            << "[TestServer] User logged in: "
            << request->username()
            << std::endl;
    } else {
        std::cout
            << "[TestServer] Login rejected: user="
            << request->username()
            << ", code="
            << response->error_code()
            << ", message="
            << result.message
            << std::endl;
    }

    return grpc::Status::OK;
}

std::uint32_t UserServiceImpl::RegisterErrorCode(
    StoreError error) noexcept
{
    switch (error) {
    case StoreError::kSuccess:
        return 0;

    case StoreError::kUserAlreadyExists:
        return 1001;

    case StoreError::kInvalidArgument:
        return 1002;

    default:
        return 1099;
    }
}

std::uint32_t UserServiceImpl::LoginErrorCode(
    StoreError error) noexcept
{
    switch (error) {
    case StoreError::kSuccess:
        return 0;

    case StoreError::kUserNotFound:
        return 2001;

    case StoreError::kInvalidPassword:
        return 2002;

    case StoreError::kInvalidArgument:
        return 2003;

    default:
        return 2099;
    }
}

} // namespace VCE::TEST_SERVER