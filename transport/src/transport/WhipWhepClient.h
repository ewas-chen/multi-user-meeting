#pragma once

#include "TransportDefine.h"

#include <optional>
#include <string>

namespace TRANSPORT {

/**
 * @brief WHIP和WHEP公共请求参数
 */
struct WhipWhepBaseConfig {
    /*
     * 完整的服务端接口地址，必须包含协议、主机、可选端口和路径。
     *
     * 示例：
     *   http://192.168.1.10:1985/rtc/v1/whip/
     *   https://media.example.com/rtc/v1/whep/
     *
     * 不在代码中硬编码http、端口和服务器地址。
     */
    std::string endpoint_url;

    // 会议房间ID。
    std::string room_id;

    // 服务端使用的应用名称，例如SRS常见的live。
    std::string app_name{"live"};

    // WHIP和WHEP共同使用的SRS公网媒体地址
    std::string rtc_external_address;

    /*
     * 可选的Bearer Token。
     * 非空时通过Authorization请求头发送：
     *
     * Authorization: Bearer <token>
     *
     * 不再将密钥硬编码在源码中。
     */
    std::string authorization_token;
};

/**
 * @brief WHIP发布配置
 */
struct PublishConfig : public WhipWhepBaseConfig {
    // 当前推流用户ID。
    std::string local_user_id;

    // Oryx WHIP推流密钥，通过URL查询参数传递
    std::string secret;
};

/**
 * @brief WHEP订阅配置
 */
struct SubscribeConfig : public WhipWhepBaseConfig {
    // 需要订阅的远端用户ID。
    std::string remote_user_id;
};

/**
 * @brief WHIP/WHEP成功响应
 */
struct WhipWhepResponse {
    // 服务端返回的Answer SDP。
    std::string answer_sdp;

    /*
     * 服务端通过Location响应头返回的资源地址。
     * 停止推流或订阅时，需要向该地址发送DELETE请求。
     *
     * 实现中应将相对地址转换为绝对地址后再保存。
     */
    std::string resource_url;

    // HTTP响应状态码，便于日志和问题定位。
    long status_code{0};
};

/**
 * @brief WHIP/WHEP HTTP信令客户端
 *
 * 负责：
 * 1. 通过HTTP POST发送本地Offer SDP。
 * 2. 接收服务端返回的Answer SDP。
 * 3. 解析Location资源地址。
 * 4. 通过HTTP DELETE停止发布或订阅。
 *
 * 不负责：
 * 1. 创建PeerConnection。
 * 2. 生成或解析RTC媒体Track。
 * 3. 传输真正的音视频数据。
 *
 * 当前接口是同步阻塞接口，因此不能直接在OBS回调、
 * libdatachannel回调或音频播放回调中调用。
 * 后续应由TransportEngine的信令工作线程调用。
 */
class TRANSPORT_ENGINE_LOCAL WhipWhepClient final {
public:
    WhipWhepClient();
    ~WhipWhepClient();

    WhipWhepClient(const WhipWhepClient&) = delete;
    WhipWhepClient& operator=(const WhipWhepClient&) = delete;
    WhipWhepClient(WhipWhepClient&&) = delete;
    WhipWhepClient& operator=(WhipWhepClient&&) = delete;

    /**
     * @brief 使用WHIP发布本地音视频
     *
     * 向WHIP地址POST本地Offer SDP，成功后返回服务端的
     * Answer SDP和用于停止推流的资源地址。
     */
    std::optional<WhipWhepResponse> Publish(
        const PublishConfig& config,
        const std::string& offer_sdp);

    /**
     * @brief 停止WHIP发布
     *
     * 向Publish()返回的resource_url发送DELETE请求。
     */
    bool Unpublish(
        const std::string& resource_url,
        const PublishConfig& config);

    /**
     * @brief 使用WHEP订阅远端用户音视频
     *
     * 向WHEP地址POST本地Offer SDP，成功后返回服务端的
     * Answer SDP和用于取消订阅的资源地址。
     */
    std::optional<WhipWhepResponse> Subscribe(
        const SubscribeConfig& config,
        const std::string& offer_sdp);

    /**
     * @brief 取消WHEP订阅
     *
     * 向Subscribe()返回的resource_url发送DELETE请求。
     */
    bool Unsubscribe(
        const std::string& resource_url,
        const SubscribeConfig& config);

private:
    /**
     * @brief 发送公共的SDP POST请求
     *
     * WHIP与WHEP的HTTP请求过程相同，因此共用这一实现，
     * 避免Publish()和Subscribe()重复编写libcurl代码。
     */
    std::optional<WhipWhepResponse> PostOffer(
        const std::string& request_url,
        const std::string& authorization_token,
        const std::string& offer_sdp);

    /**
     * @brief 向资源地址发送DELETE请求
     */
    bool DeleteResource(
        const std::string& resource_url,
        const std::string& authorization_token);

    /**
     * @brief 根据房间和本地用户构造WHIP发布地址
     */
    static std::string BuildPublishEndpoint(
        const PublishConfig& config);

    /**
     * @brief 根据房间和远端用户构造WHEP订阅地址
     */
    static std::string BuildSubscribeEndpoint(
        const SubscribeConfig& config);

    /**
     * @brief 将Location中的相对资源地址转换为绝对地址
     *
     * 例如：
     *   请求地址：http://server:1985/rtc/v1/whip/
     *   Location：/rtc/session/123
     *
     * 转换后：
     *   http://server:1985/rtc/session/123
     */
    static std::string ResolveResourceUrl(
        const std::string& request_url,
        const std::string& location);
};

} // namespace TRANSPORT