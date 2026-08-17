#pragma once

#include "ICaptureEngine.h"

#include <cstdint>
#include <string>

/*
 * Linux ELF动态库符号可见性。
 * 建议配合编译选项 -fvisibility=hidden 使用。
 */
#define TRANSPORT_ENGINE_API __attribute__((visibility("default")))
    
#define TRANSPORT_ENGINE_LOCAL __attribute__((visibility("hidden")))

namespace TRANSPORT {
static int m_video_queue_drop_count = 0;


/*
 * 直接复用采集模块的公共帧类型。
 *
 * CaptureEngine产生的帧可以直接交给TransportEngine，
 * TransportEngine解码后的帧也可以直接交给RenderEngine，
 * 不需要重新分配内存或进行结构转换。
 */
using I420Frame = CAPTURE::I420Frame;
using AudioFrame = CAPTURE::AudioFrame;

/**
 * WebRTC连接状态
 *
 * 已有枚举值保持稳定，后续新增状态只能向后追加。
 */
enum class ConnectionState : std::uint8_t
{
    kDisconnected = 0,
    kConnecting,
    kConnected,
    kDisconnecting,
    kFailed,
    kClosed
};

/**
 * 传输模块操作结果
 */
enum class TransportResult : std::uint8_t
{
    SUCCESS = 0,
    ERROR_NOT_INITIALIZED,
    ERROR_INTERNAL,
    ERROR_FAILED_TO_PUSH_STREAM,
    ERROR_NOT_CONNECTED,
    ERROR_ALREADY_SUBSCRIBED,
    ERROR_NOT_SUBSCRIBED,
    ERROR_STREAM_NOT_AVAILABLE
};

/**
 * 本地音视频发布参数
 *
 * 参数应当与CaptureEngine的输出配置保持一致。
 */
struct PublishInfo
{
    int video_width{0};
    int video_height{0};
    int video_fps{0};

    int audio_sample_rate{0};
    int audio_channels{0};
};

/**
 * 目标会议房间信息
 */
struct TransportTargetRoomInfo
{
    /*
     * WHIP/WHEP服务基础地址。
     * 后续实现不应在代码中硬编码协议和端口。
     */
    std::string push_server_url;
    std::string pull_server_url;

    std::string room_id;
    std::string local_user_id;
    std::string app_name{"live"};
    
    // Oryx推流密钥，仅WHIP使用
    std::string whip_secret;

    // SRS对外提供WebRTC服务的地址，例如82.156.137.234:8000
    std::string rtc_external_address;
};

} // namespace TRANSPORT