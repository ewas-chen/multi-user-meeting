#pragma once

#include "TransportDefine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <rtc/rtc.hpp>

namespace TRANSPORT {

// 这是本项目在SDP中使用的动态RTP Payload Type，不是协议强制值。
// 后续创建Track和RTP Packetizer时必须保持一致。
inline constexpr std::uint8_t kH264PayloadType = 96;
inline constexpr std::uint8_t kOpusPayloadType = 111;

// RtcPullTransport解码完成后，通过这些回调输出公共音视频帧。
using OnVideoFrameCallback =
    std::function<void(const std::shared_ptr<I420Frame>& frame)>;

using OnAudioFrameCallback =
    std::function<void(const std::shared_ptr<AudioFrame>& frame)>;

// PeerConnection连接状态变化回调。
using OnRtcStateChangeCallback =
    std::function<void(ConnectionState state)>;

/*
 * ICE候选收集(可能用于建立连接的网络地址)完成后产生本地SDP(一段描述音视频连接信息的文本)
 *
 * WHIP/WHEP采用HTTP交换完整SDP，当前实现使用非Trickle ICE流程(等所有候选都收集完，再把它们一起放进 SDP，并一次性发送)，
 * 因此需要等待GatheringState::Complete后再发送Offer。
 */
using OnLocalDescriptionCallback =
    std::function<void(const rtc::Description& description)>;

/**
 * @brief RTC推流和拉流的公共基类
 * 当前只发送一次 HTTP 请求，因此必须等所有网络候选地址都写入 SDP 后，再把完整 Offer 发给服务器
 *
 * 负责：
 * 1. 创建和关闭rtc::PeerConnection。
 * 2. 监听PeerConnection及ICE状态。
 * 3. 在ICE候选收集完成后返回完整的本地SDP。
 * 4. 设置WHIP/WHEP服务端返回的远端SDP。
 * 5. 保存公共发布参数和本地用户ID。
 *
 * 不负责：
 * 1. 创建音视频Track。
 * 2. 音视频编解码。
 * 3. RTP封包和解包。
 * 4. WHIP/WHEP HTTP请求。
 *
 * RtcPushTransport和RtcPullTransport分别继承此类，
 * 完成具体的发送和接收工作。
 */
class TRANSPORT_ENGINE_LOCAL RtcTransportBase {
public:
    RtcTransportBase();
    virtual ~RtcTransportBase();

    RtcTransportBase(const RtcTransportBase&) = delete;
    RtcTransportBase& operator=(const RtcTransportBase&) = delete;
    RtcTransportBase(RtcTransportBase&&) = delete;
    RtcTransportBase& operator=(RtcTransportBase&&) = delete;

    /**
     * @brief 保存音视频参数
     *
     * 这里只保存参数，不创建PeerConnection，也不会连接服务器。
     */
    bool Initialize(const PublishInfo& publish_info);

    /**
     * @brief 创建PeerConnection并注册RTC状态回调
     *
     * Open只建立RTC基础环境。必须先创建音视频Track(可以理解为 WebRTC 连接中的一条“媒体传输通道”)，
     * 然后才能调用setLocalDescription()生成正确的Offer。
     */
    bool Open();

    /**
     * @brief 关闭具体的推流或拉流对象
     *
     * 派生类需要先释放Track、编解码器和工作线程，
     * 最后调用ClosePeerConnection()释放公共RTC资源。
     */
    virtual void Close() = 0;

    /**
     * @brief 设置WHIP/WHEP服务端返回的远端SDP
     *
     * @param sdp SDP文本。
     * @param type SDP类型，通常为"answer"。
     */
    bool SetRemoteDescription(const std::string& sdp, const std::string& type);

    bool IsOpen() const noexcept;
    ConnectionState GetState() const noexcept;

    void SetStateChangeCallback(OnRtcStateChangeCallback callback);
    void SetLocalDescriptionCallback(OnLocalDescriptionCallback callback);

    PublishInfo GetPublishInfo() const;

    // 设置当前RTC对象对应的本地用户或远端订阅用户。
    void SetUserId(const std::string& user_id);
    std::string GetUserId() const;

protected:
    /**
     * @brief 更新连接状态并通知上层
     *
     * 回调可能运行在libdatachannel内部线程，
     * 实现时不要在持有内部互斥锁的情况下执行外部回调。
     */
    void SetState(ConnectionState state);

    /**
     * @brief 释放公共PeerConnection资源
     *
     * 派生类Close()完成Track清理后调用。
     */
    void ClosePeerConnection() noexcept;

private:
    // 将libdatachannel状态转换为项目公共ConnectionState。
    void HandleConnectionState(rtc::PeerConnection::State state);

    // ICE收集完成后返回包含全部候选地址的本地SDP。
    void HandleGatheringState(
        rtc::PeerConnection::GatheringState state);

protected:
    /*
     * 派生类需要使用PeerConnection创建音视频Track，
     * 但不应该直接reset；统一由ClosePeerConnection()释放。
     */
    std::shared_ptr<rtc::PeerConnection> m_peer_connection;

private:
    mutable std::mutex m_state_mutex;
    mutable std::mutex m_callback_mutex;

    PublishInfo m_publish_info;
    std::string m_user_id;

    std::atomic<ConnectionState> m_state{
        ConnectionState::kDisconnected
    };

    OnRtcStateChangeCallback m_state_change_callback;
    OnLocalDescriptionCallback m_local_description_callback;

    bool m_initialized{false};
};

} // namespace TRANSPORT