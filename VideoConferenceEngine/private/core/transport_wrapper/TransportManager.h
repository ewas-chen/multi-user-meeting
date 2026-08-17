#pragma once

#include "ITransportDataCallback.h"
#include "VceTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace TRANSPORT {

class ITransportEngine;
enum class ConnectionState : std::uint8_t;
} // namespace TRANSPORT

namespace VCE {

/**
 * @brief 传输模块初始化参数
 */
struct PublishConfig
{
    int video_width{0};
    int video_height{0};
    int video_fps{0};

    int audio_sample_rate{0};
    int audio_channels{0};
};

/**
 * @brief 当前会议的媒体传输配置
 */
struct RoomInfo
{
    std::string local_user_id;
    std::string room_id;
    MediaServerInfo media_server;
};

/**
 * @brief RTC传输模块包装器
 *
 * TransportManager负责：
 * - 管理TransportEngine生命周期；
 * - 设置WHIP/WHEP会议配置；
 * - 控制本地音视频发布；
 * - 管理远端用户订阅；
 * - 转发本地原始音视频帧；
 * - 将远端解码帧和连接状态回调给VceEngineImpl。
 *
 * TransportEngine内部已经具有编码线程、发送队列、RTP接收缓冲
 * 和解码线程，因此Manager不再增加额外消费者线程和媒体队列。
 *
 * 会议层与TransportEngine使用相同的I420Frame和AudioFrame类型，
 * 音视频数据可以直接传递，不需要转换或复制。
 */
class TransportManager
{
public:
    TransportManager();
    ~TransportManager();

    TransportManager(const TransportManager&) = delete;
    TransportManager& operator=(const TransportManager&) = delete;
    TransportManager(TransportManager&&) = delete;
    TransportManager& operator=(TransportManager&&) = delete;

    // ==================== 生命周期 ====================

    Result Initialize(const PublishConfig& config);

    void Uninit();

    // ==================== 房间配置 ====================

    /**
     * @brief 设置当前会议的WHIP/WHEP传输信息
     *
     * 应在开始发布和订阅之前调用。
     */
    Result SetRoomInfo(const RoomInfo& room_info);

    RoomInfo GetRoomInfo() const;

    // ==================== 本地音视频发布 ====================

    Result StartPublishCameraVideo();
    Result StopPublishCameraVideo();

    Result StartPublishAudio();
    Result StopPublishAudio();

    /**
     * @brief 将本地I420帧交给传输模块编码和发送
     *
     * capture_type当前主要为摄像头，保留该参数以兼容源码结构
     * 并为后续屏幕共享等视频来源预留扩展位置。
     */
    Result PushVideoFrame(const std::shared_ptr<I420Frame>& frame,
                          CaptureType capture_type);

    /**
     * @brief 将本地Float32 PCM帧交给传输模块编码和发送
     */
    Result PushAudioFrame(const std::shared_ptr<AudioFrame>& frame);

    bool IsPublishingCameraVideo();
    bool IsPublishingAudio();

    // ==================== 远端用户订阅 ====================

    Result SubscribeUserAV(const std::string& user_id);
    Result UnsubscribeUserAV(const std::string& user_id);

    Result SubscribeGroupUsers(const std::vector<std::string>& user_ids);
    Result UnsubscribeGroupUsers(const std::vector<std::string>& user_ids);

    bool IsUserSubscribedAV(const std::string& user_id);
    std::vector<std::string> GetSubscribedUsers();

    // ==================== 上层回调 ====================

    /**
     * @brief 设置远端媒体和连接状态接收对象
     *
     * 使用weak_ptr保存回调对象，避免TransportManager与
     * VceEngineImpl之间形成shared_ptr循环引用。
     */
    Result SetTransportDataCallback(
        const std::shared_ptr<ITransportDataCallback>& callback);

private:
    /**
     * @brief 接收TransportEngine解码后的视频帧
     */
    void OnVideoFrameReceived(
        const std::string& user_id,
        const std::shared_ptr<I420Frame>& frame);

    /**
     * @brief 接收TransportEngine解码后的音频帧
     */
    void OnAudioFrameReceived(
        const std::string& user_id,
        const std::shared_ptr<AudioFrame>& frame);

    /**
     * @brief 接收TransportEngine连接状态
     */
    void OnConnectionStateChanged(
        TRANSPORT::ConnectionState state);

    /**
     * @brief 将传输模块状态转换为会议引擎公开状态
     */
    static TransportState ConvertConnectionState(
        TRANSPORT::ConnectionState state) noexcept;

private:
    /*
     * 保护TransportEngine生命周期和房间配置。
     * 初始化、反初始化、发布和订阅操作不能同时修改引擎。
     */
    mutable std::mutex m_state_mutex;

    std::unique_ptr<TRANSPORT::ITransportEngine> m_transport_engine;
    RoomInfo m_room_info;

    /*
     * 只保护回调对象本身。
     * 调用外部回调前应先取得shared_ptr并释放此锁。
     */
    mutable std::mutex m_callback_mutex;
    std::weak_ptr<ITransportDataCallback> m_transport_callback;

    std::atomic<bool> m_initialized{false};
};

} // namespace VCE