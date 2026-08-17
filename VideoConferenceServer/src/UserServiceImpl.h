#pragma once

#include "InMemoryMeetingStore.h"
#include "user_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>

namespace VCE::TEST_SERVER {

/**
 * @brief 本地测试用用户gRPC服务
 *
 * 只负责将Register和Login请求转换为InMemoryMeetingStore操作。
 * 业务失败仍返回grpc::Status::OK，具体错误通过proto中的
 * error_code和error_message返回，与当前客户端约定保持一致。
 */
class UserServiceImpl final
    : public ::user_service::UserService::Service {
public:
    explicit UserServiceImpl(
        std::shared_ptr<InMemoryMeetingStore> store);

    ~UserServiceImpl() override = default;

    UserServiceImpl(const UserServiceImpl&) = delete;
    UserServiceImpl& operator=(const UserServiceImpl&) = delete;
    UserServiceImpl(UserServiceImpl&&) = delete;
    UserServiceImpl& operator=(UserServiceImpl&&) = delete;

    grpc::Status Register(
        grpc::ServerContext* context,
        const ::user_service::RegisterRequest* request,
        ::user_service::RegisterResponse* response) override;

    grpc::Status Login(
        grpc::ServerContext* context,
        const ::user_service::LoginRequest* request,
        ::user_service::LoginResponse* response) override;

private:
    static std::uint32_t RegisterErrorCode(
        StoreError error) noexcept;

    static std::uint32_t LoginErrorCode(
        StoreError error) noexcept;

private:
    std::shared_ptr<InMemoryMeetingStore> m_store;
};

} // namespace VCE::TEST_SERVER