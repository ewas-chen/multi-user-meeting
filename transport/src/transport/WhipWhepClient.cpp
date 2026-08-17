#include "WhipWhepClient.h"

#include "utils/logManager.h"

#include <curl/curl.h>
#include <strings.h>

#include <string>
#include <utility>

namespace {

constexpr long kConnectTimeoutMs = 5000;
constexpr long kRequestTimeoutMs = 15000;

struct CurlResponse {
    std::string body;
    std::string location;
};

// 接收HTTP响应正文，例如服务端返回的Answer SDP。
std::size_t WriteBodyCallback(
    char* data,
    std::size_t size,
    std::size_t count,
    void* user_data) noexcept {
    const std::size_t data_size = size * count;
    auto* response = static_cast<CurlResponse*>(user_data);

    if (!data || !response) {
        return 0;
    }

    try {
        response->body.append(data, data_size);
    } catch (...) {
        return 0;
    }

    return data_size;
}

// 从HTTP响应头中读取Location资源地址。
std::size_t WriteHeaderCallback(
    char* data,
    std::size_t size,
    std::size_t count,
    void* user_data) noexcept {
    const std::size_t data_size = size * count;
    auto* response = static_cast<CurlResponse*>(user_data);

    if (!data || !response) {
        return 0;
    }

    constexpr std::size_t location_prefix_size = 9;

    if (data_size <= location_prefix_size ||
        strncasecmp(data, "Location:", location_prefix_size) != 0) {
        return data_size;
    }

    try {
        std::string location(
            data + location_prefix_size,
            data_size - location_prefix_size);

        while (!location.empty() &&
               (location.front() == ' ' || location.front() == '\t')) {
            location.erase(location.begin());
        }

        while (!location.empty() &&
               (location.back() == ' ' ||
                location.back() == '\t' ||
                location.back() == '\r' ||
                location.back() == '\n')) {
            location.pop_back();
        }

        response->location = std::move(location);
    } catch (...) {
        return 0;
    }

    return data_size;
}

} // namespace

namespace TRANSPORT {

WhipWhepClient::WhipWhepClient() {
    const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK) {
        LOG_ERROR("Failed to initialize libcurl: {}", curl_easy_strerror(result));
    }
}

WhipWhepClient::~WhipWhepClient() {
    /*
     * 当前TransportEngine只创建一个WhipWhepClient。
     * 析构前必须先停止并等待所有信令请求结束。
     */
    curl_global_cleanup();
}

std::optional<WhipWhepResponse> WhipWhepClient::Publish(
    const PublishConfig& config,
    const std::string& offer_sdp) {
    if (config.local_user_id.empty()) {
        LOG_ERROR("WHIP publish failed: local_user_id is empty");
        return std::nullopt;
    }

    const std::string endpoint = BuildPublishEndpoint(config);
    if (endpoint.empty()) {
        LOG_ERROR("WHIP publish failed: invalid endpoint configuration");
        return std::nullopt;
    }

    LOG_INFO("Sending WHIP offer: room={}, user={}",
             config.room_id, config.local_user_id);

    return PostOffer(endpoint, config.authorization_token, offer_sdp);
}

bool WhipWhepClient::Unpublish(
    const std::string& resource_url,
    const PublishConfig& config) {
    return DeleteResource(resource_url, config.authorization_token);
}

std::optional<WhipWhepResponse> WhipWhepClient::Subscribe(
    const SubscribeConfig& config,
    const std::string& offer_sdp) {
    if (config.remote_user_id.empty()) {
        LOG_ERROR("WHEP subscribe failed: remote_user_id is empty");
        return std::nullopt;
    }

    const std::string endpoint = BuildSubscribeEndpoint(config);
    if (endpoint.empty()) {
        LOG_ERROR("WHEP subscribe failed: invalid endpoint configuration");
        return std::nullopt;
    }

    LOG_INFO("Sending WHEP offer: room={}, remote_user={}",
             config.room_id, config.remote_user_id);

    auto result = PostOffer(endpoint, config.authorization_token, offer_sdp);
        
    return result;
}

bool WhipWhepClient::Unsubscribe(
    const std::string& resource_url,
    const SubscribeConfig& config) {
    return DeleteResource(resource_url, config.authorization_token);
}

std::optional<WhipWhepResponse> WhipWhepClient::PostOffer(
    const std::string& request_url,
    const std::string& authorization_token,
    const std::string& offer_sdp) {
    if (request_url.empty() || offer_sdp.empty()) {
        LOG_ERROR("Cannot send SDP offer: URL or SDP is empty");
        return std::nullopt;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to create libcurl handle");
        return std::nullopt;
    }

    CurlResponse response;
    curl_slist* headers = nullptr;

    headers = curl_slist_append(headers, "Content-Type: application/sdp");
    headers = curl_slist_append(headers, "Accept: application/sdp");

    if (!authorization_token.empty()) {
        const std::string authorization =
            "Authorization: Bearer " + authorization_token;
        headers = curl_slist_append(headers, authorization.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, offer_sdp.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(offer_sdp.size()));

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // libcurl默认开启HTTPS证书与主机名校验，不在这里关闭。
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);

    /*
    * WebRTC媒体连接本身不会经过HTTP代理，
    * WHIP/WHEP信令也直接访问指定媒体服务器。
    */
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");

    const CURLcode curl_result = curl_easy_perform(curl);

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_result != CURLE_OK) {
        LOG_ERROR("WHIP/WHEP request failed: {}",
                  curl_easy_strerror(curl_result));
        return std::nullopt;
    }

    if (status_code < 200 || status_code >= 300) {
        LOG_ERROR("WHIP/WHEP request rejected: status={}, body={}",
                  status_code, response.body);
        return std::nullopt;
    }

    if (response.body.empty()) {
        LOG_ERROR("WHIP/WHEP response does not contain Answer SDP");
        return std::nullopt;
    }

    WhipWhepResponse result;
    result.answer_sdp = std::move(response.body);
    result.status_code = status_code;

    if (!response.location.empty()) {
        result.resource_url =
            ResolveResourceUrl(request_url, response.location);
    } else {
        LOG_WARN("WHIP/WHEP response does not contain Location");
    }

    LOG_INFO("WHIP/WHEP SDP exchange succeeded: status={}", status_code);
    return result;
}

bool WhipWhepClient::DeleteResource(
    const std::string& resource_url,
    const std::string& authorization_token) {
    if (resource_url.empty()) {
        LOG_ERROR("Cannot delete RTC resource: URL is empty");
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to create libcurl handle");
        return false;
    }

    CurlResponse response;
    curl_slist* headers = nullptr;

    if (!authorization_token.empty()) {
        const std::string authorization =
            "Authorization: Bearer " + authorization_token;
        headers = curl_slist_append(headers, authorization.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, resource_url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    const CURLcode curl_result = curl_easy_perform(curl);

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_result != CURLE_OK) {
        LOG_ERROR("DELETE request failed: {}",
                  curl_easy_strerror(curl_result));
        return false;
    }

    if (status_code < 200 || status_code >= 300) {
        LOG_ERROR("DELETE request rejected: status={}, body={}",
                  status_code, response.body);
        return false;
    }

    LOG_INFO("RTC server resource deleted: status={}", status_code);
    return true;
}

std::string WhipWhepClient::BuildPublishEndpoint(
    const PublishConfig& config) {
    if (config.endpoint_url.empty() ||
        config.app_name.empty() ||
        config.room_id.empty() ||
        config.local_user_id.empty()) {
        return {};
    }

    std::string endpoint = config.endpoint_url;

    if (endpoint.find('?') == std::string::npos) {
        endpoint += '?';
    } else if (endpoint.back() != '?' &&
               endpoint.back() != '&') {
        endpoint += '&';
    }

    endpoint += "app=" + config.app_name;
    endpoint += "&stream=" +
                config.room_id + "_" +
                config.local_user_id;

    if (!config.rtc_external_address.empty()) {
        endpoint += "&eip=" +
                    config.rtc_external_address;
    }

    if (!config.secret.empty()) {
        endpoint += "&secret=" +
                    config.secret;
    }

    return endpoint;
}

std::string WhipWhepClient::BuildSubscribeEndpoint(
    const SubscribeConfig& config) {
    if (config.endpoint_url.empty() ||
        config.app_name.empty() ||
        config.room_id.empty() ||
        config.remote_user_id.empty()) {
        return {};
    }

    std::string endpoint = config.endpoint_url;

    if (endpoint.find('?') == std::string::npos) {
        endpoint += '?';
    } else if (endpoint.back() != '?' &&
               endpoint.back() != '&') {
        endpoint += '&';
    }

    endpoint += "app=" + config.app_name;
    endpoint += "&stream=" +
                config.room_id + "_" +
                config.remote_user_id;

    if (!config.rtc_external_address.empty()) {
        endpoint += "&eip=" +
                    config.rtc_external_address;
    }

    LOG_INFO(
        "Sending WHEP offer: url={}, room={}, remote_user={}",
        endpoint,
        config.room_id,
        config.remote_user_id);

    return endpoint;
}

std::string WhipWhepClient::ResolveResourceUrl(
    const std::string& request_url,
    const std::string& location) {
    if (request_url.empty() || location.empty()) {
        return {};
    }

    // Location已经是完整地址。
    if (location.rfind("http://", 0) == 0 ||
        location.rfind("https://", 0) == 0) {
        return location;
    }

    const std::size_t scheme_end = request_url.find("://");
    if (scheme_end == std::string::npos) {
        return {};
    }

    // 处理形如//server/path的地址。
    if (location.rfind("//", 0) == 0) {
        return request_url.substr(0, scheme_end) + ":" + location;
    }

    const std::size_t authority_begin = scheme_end + 3;
    const std::size_t authority_end =
        request_url.find_first_of("/?#", authority_begin);

    const std::string origin =
        authority_end == std::string::npos
            ? request_url
            : request_url.substr(0, authority_end);

    // 处理形如/rtc/session/123的根路径。
    if (location.front() == '/') {
        return origin + location;
    }

    // 处理形如session/123的相对路径。
    std::string base_url = request_url;

    const std::size_t query = base_url.find_first_of("?#");
    if (query != std::string::npos) {
        base_url.erase(query);
    }

    const std::size_t last_slash = base_url.rfind('/');
    if (last_slash == std::string::npos ||
        last_slash < authority_begin) {
        return origin + "/" + location;
    }

    return base_url.substr(0, last_slash + 1) + location;
}

} // namespace TRANSPORT