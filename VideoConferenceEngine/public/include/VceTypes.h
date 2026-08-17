#pragma once

#include "ICaptureEngine.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace VCE {

/**
 * @brief 业务服务及媒体服务器配置
 *
 * 当前服务端只返回媒体服务器IPv4地址，
 * WHIP/WHEP路径、端口、应用名和密钥由客户端补全。
 */
struct ServiceConfig
{
    // 用户和会议gRPC服务地址，例如82.156.137.234:50051
    std::string server_address;

    /*
     * 当前客户端的公网IPv4地址。
     * 服务端源码使用该地址进行媒体服务器调度。
     */
    std::string client_ip;

    // 单次gRPC请求超时时间
    std::chrono::milliseconds request_timeout{
        std::chrono::milliseconds{5000}};

    // SRS WHIP/WHEP信令使用的HTTP配置
    std::string media_http_scheme{"http"};
    std::uint16_t media_http_port{80};

    // SRS WebRTC媒体传输UDP端口
    std::uint16_t media_rtc_port{8000};

    std::string whip_path{"/rtc/v1/whip/"};
    std::string whep_path{"/rtc/v1/whep/"};

    // SRS应用名称，通常为live
    std::string app_name{"live"};

    /*
     * 当前服务端proto没有返回SRS发布密钥，
     * 因此暂时由客户端配置提供。
     */
    std::string publish_secret;
};

struct EngineConfig
{
    int sample_rate{48000};
    int channels{2};

    int video_width{640};
    int video_height{480};
    int video_fps{30};

    ServiceConfig service;
};

/*
 * 会议引擎直接复用三个媒体模块约定的公共帧类型。
 * 模块间传递shared_ptr时不会复制I420或PCM数据。
 */
using I420Frame = CAPTURE::I420Frame;
using AudioFrame = CAPTURE::AudioFrame;

// 会议层采集源类型，避免公共接口直接暴露底层OBS源类型。
enum class CaptureType : std::int32_t
{
    kCT_Unknown = 0,
    kCT_Mic,
    kCT_Camera
};

// VideoConferenceEngine公共接口返回值。
enum Result : std::int32_t
{
    kRet_SUCCESS = 0,

    // 业务服务
    kRet_Error_Response = -0x01,
    kRet_Invalid_Status = -0x02,
    kRet_NoClientIp = -0x03,

    // 公共错误
    kRet_InvalidParam = -0x10,
    kRet_InvalidFileDirectory = -0x11,

    // 采集模块
    kRet_CaptureFailedToInit = -0x60,
    kRet_CaptureNotInitialized = -0x61,
    kRet_CaptureFailedToCreateSource = -0x62,
    kRet_CaptureNoCurrentSource = -0x63,
    kRet_CaptureFailedToCreateProperty = -0x64,
    kRet_CaptureFailedToSetProperty = -0x65,
    kRet_CaptureFailedToGetDevice = -0x66,

    // 渲染模块
    kRet_RenderFailedToInit = -0x80,
    kRet_RenderNotInitialized = -0x81,
    kRet_RenderFailedToAddUser = -0x82,
    kRet_RenderFailedToRemoveUser = -0x83,
    kRet_RenderFailedToUpdateVideo = -0x84,
    kRet_RenderFailedToUpdateAudio = -0x85,
    kRet_RenderFailedToSetWindow = -0x86,
    kRet_RenderFailedToResize = -0x87,
    kRet_RenderFailedToEnumSpeakers = -0x88,
    kRet_RenderFailedToSetSpeaker = -0x89,
    kRet_RenderFailedToGetSpeaker = -0x8A,
    kRet_RenderFailedToSetSyncThreshold = -0x8B,
    kRet_RenderFailedToGetSyncThreshold = -0x8C,
    kRet_RenderFailedToGetStats = -0x8D,
    kRet_RenderFailedToResetStats = -0x8E,

    // 传输模块
    kRet_TransportAlreadyInitialized = -0x100,
    kRet_TransportFailedToConnectServer = -0x101,
    kRet_TransportNotInitialized = -0x102,
    kRet_TransportFailedToSetRoom = -0x103,
    kRet_TransportFailedToPublish = -0x104,
    kRet_TransportFailedToSubscribe = -0x105
};

struct UserInfo
{
    std::string user_name;
    std::string client_ip;
};

struct MeetingBriefInfo
{
    std::string user_name;
    std::string meeting_id;
};

struct MeetingInfo
{
    std::string meeting_id;
    std::string title;
    std::string description;

    std::int64_t start_time{0};
    std::int64_t duration{0};

    std::string creator_user_name;
    std::uint32_t creator_client_ip{0};

    std::int32_t participant_count{0};
    bool is_active{false};
};

struct CreateMeetingInfo
{
    std::string user_name;
    std::string title;
    std::string description;
    std::int64_t start_time{0};
};

/*
 * SRS媒体服务器连接信息。
 *
 * room_id和local_user_id来自会议请求，
 * 不在媒体服务器配置中重复保存。
 */
struct MediaServerInfo
{
    // WHIP信令地址
    std::string push_server_url;

    // WHEP信令地址
    std::string pull_server_url;

    // SRS应用名称，通常为live
    std::string app_name{"live"};

    // WebRTC公网候选地址，例如82.156.137.234:8000
    std::string rtc_external_address;

    // WHIP推流地址所需的SRS密钥
    std::string publish_secret;
};

struct CreateMeetingResponse
{
    std::string meeting_id;
    MediaServerInfo media_server;
};

struct JoinMeetingResponse
{
    std::string meeting_title;
    std::string meeting_description;

    // 当前已经在会议中的用户
    std::vector<UserInfo> participants;

    MediaServerInfo media_server;
};

struct GetMeetingListRequest
{
    std::string user_name;
    std::int32_t page_size{10};
    std::int32_t page_number{1};
};

struct GetMeetingListResponse
{
    std::vector<MeetingInfo> meetings;
    std::int32_t total_count{0};
    std::int32_t total_pages{1};
};

struct CameraDeviceInfo
{
    std::string name;
    std::string id;
};

struct MicDeviceInfo
{
    std::string name;
    std::string id;
};

struct SpeakerDeviceInfo
{
    std::string name;
    std::string id;
    bool is_default{false};
};

// 对外屏蔽传输模块内部的ConnectionState类型。
enum class TransportState : std::int32_t
{
    kDisconnected = 0,
    kConnecting,
    kConnected,
    kReconnecting,
    kDisconnecting,
    kFailed,
    kClosed
};

} // namespace VCE