#pragma once

#include "VceTypes.h"
#include "user_service.grpc.pb.h"

#include <chrono>
#include <memory>
#include <string>

namespace VCE::SERVICE {

/**
 * @brief 用户业务gRPC客户端
 *
 * 负责调用服务端的用户注册和登录接口，
 * 并将gRPC状态及服务端业务错误转换为VCE::Result。
 *
 * protobuf类型只在Service模块内部使用，不向其他模块暴露。
 */
class UserService final {
public:
    explicit UserService(
        const std::string& server_address,
        std::chrono::milliseconds request_timeout =
            std::chrono::milliseconds{5000});

    ~UserService() = default;

    UserService(const UserService&) = delete;
    UserService& operator=(const UserService&) = delete;
    UserService(UserService&&) = delete;
    UserService& operator=(UserService&&) = delete;

    /**
     * @brief 注册用户
     *
     * 密码不在客户端进行MD5处理，服务端源码会负责密码哈希。
     */
    Result RegisterUser(const std::string& user_name,
                        const std::string& password);

    /**
     * @brief 登录用户
     */
    Result LoginUser(const std::string& user_name, const std::string& password);

private:
    std::unique_ptr<user_service::UserService::Stub> m_stub;
    std::chrono::milliseconds m_request_timeout;
};

} // namespace VCE::SERVICE