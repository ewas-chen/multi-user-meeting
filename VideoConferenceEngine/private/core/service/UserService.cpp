#include "UserService.h"

#include "utils/logManager.h"

#include <grpcpp/grpcpp.h>

namespace VCE::SERVICE {

UserService::UserService(
    const std::string& server_address,
    std::chrono::milliseconds request_timeout)
    : m_request_timeout(request_timeout) {
    if (server_address.empty()) {
        LOG_ERROR("Failed to create UserService: server address is empty");
        return;
    }

    if (m_request_timeout.count() <= 0) {
        m_request_timeout = std::chrono::milliseconds{5000};
    }

    /*
     * 与当前服务端源码保持一致，使用非TLS的gRPC连接。
     * CreateChannel采用延迟连接，实际连接会在首次RPC请求时建立。
     */
    auto channel = grpc::CreateChannel(
        server_address,
        grpc::InsecureChannelCredentials());

    if (!channel) {
        LOG_ERROR("Failed to create UserService gRPC channel");
        return;
    }

    m_stub = ::user_service::UserService::NewStub(channel);

    if (!m_stub) {
        LOG_ERROR("Failed to create UserService gRPC stub");
    }
}

Result UserService::RegisterUser(
    const std::string& user_name,
    const std::string& password) {
    if (user_name.empty() || password.empty()) {
        LOG_ERROR("Register user failed: user name or password is empty");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR("Register user failed: UserService stub is unavailable");
        return kRet_Invalid_Status;
    }

    ::user_service::RegisterRequest request;
    request.set_username(user_name);
    request.set_password(password);

    ::user_service::RegisterResponse response;
    grpc::ClientContext context;

    /*
     * 防止服务端不可达时RPC调用一直阻塞。
     */
    context.set_deadline(
        std::chrono::system_clock::now() +
        m_request_timeout);

    const grpc::Status status =
        m_stub->Register(&context, request, &response);

    /*
     * gRPC状态用于表示网络连接、超时以及服务调用错误。
     */
    if (!status.ok()) {
        LOG_ERROR(
            "Register user RPC failed: user={}, grpc_code={}, message={}",
            user_name,
            static_cast<int>(status.error_code()),
            status.error_message());

        return kRet_Invalid_Status;
    }

    /*
     * 当前服务端约定error_code等于0表示成功。
     */
    if (response.error_code() != 0) {
        LOG_ERROR(
            "Register user rejected: user={}, error_code={}, message={}",
            user_name,
            response.error_code(),
            response.error_message());

        return kRet_Error_Response;
    }

    LOG_INFO("User registered successfully: {}", user_name);
    return kRet_SUCCESS;
}

Result UserService::LoginUser(
    const std::string& user_name,
    const std::string& password) {
    if (user_name.empty() || password.empty()) {
        LOG_ERROR("Login user failed: user name or password is empty");
        return kRet_InvalidParam;
    }

    if (!m_stub) {
        LOG_ERROR("Login user failed: UserService stub is unavailable");
        return kRet_Invalid_Status;
    }

    ::user_service::LoginRequest request;
    request.set_username(user_name);
    request.set_password(password);

    ::user_service::LoginResponse response;
    grpc::ClientContext context;

    context.set_deadline(
        std::chrono::system_clock::now() +
        m_request_timeout);

    const grpc::Status status =
        m_stub->Login(&context, request, &response);

    if (!status.ok()) {
        LOG_ERROR(
            "Login user RPC failed: user={}, grpc_code={}, message={}",
            user_name,
            static_cast<int>(status.error_code()),
            status.error_message());

        return kRet_Invalid_Status;
    }

    /*
     * 服务端在用户名或密码错误时仍返回gRPC OK，
     * 具体业务结果通过error_code传递。
     */
    if (response.error_code() != 0) {
        LOG_ERROR(
            "Login user rejected: user={}, error_code={}, message={}",
            user_name,
            response.error_code(),
            response.error_message());

        return kRet_Error_Response;
    }

    LOG_INFO("User logged in successfully: {}", user_name);
    return kRet_SUCCESS;
}

} // namespace VCE::SERVICE