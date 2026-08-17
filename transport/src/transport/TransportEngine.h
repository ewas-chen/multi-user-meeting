#pragma once

#include "ITransportEngine.h"
#include "WhipWhepClient.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rtc {
class Description;
}

namespace TRANSPORT {

class RtcPushTransport;
class RtcPullTransport;

/**
 * @brief Transport模块统一实现
 *
 * 负责组织以下对象：
 *
 * 一个RtcPushTransport：
 *   发布当前本地用户的音视频。
 *
 * 多个RtcPullTransport：
 *   每个远端用户对应一个拉流对象。
 *
 * 一个WhipWhepClient：
 *   负责通过HTTP交换Offer和Answer SDP。
 *
 * TransportEngine本身不执行音视频编解码，
 * 只负责模块生命周期、用户管理、信令和数据转发。
 */
class TRANSPORT_ENGINE_LOCAL TransportEngine final
    : public ITransportEngine {
private:
    /**
     * @brief 单个远端用户的拉流信息
     */
    struct PullSession {
        std::shared_ptr<RtcPullTransport> transport;

        // WHEP返回的资源地址，取消订阅时发送DELETE。
        std::string resource_url;

        // 保存请求配置，取消订阅时需要认证信息。
        SubscribeConfig config;
    };

public:
    TransportEngine();
    ~TransportEngine() override;

    TransportEngine(const TransportEngine&) = delete;
    TransportEngine& operator=(const TransportEngine&) = delete;
    TransportEngine(TransportEngine&&) = delete;
    TransportEngine& operator=(TransportEngine&&) = delete;

    /**
     * @brief 初始化传输模块
     *
     * 创建本地推流对象、HTTP信令客户端和信令线程。
     * 此时不会立即建立PeerConnection。
     */
    bool Initialize(const PublishInfo& publish_info) override;

    /**
     * @brief 反初始化传输模块
     *
     * 停止推流、取消所有订阅、停止信令线程并释放资源。
     */
    void Uninit() override;

    void SetTargetRoomInfo(
        const TransportTargetRoomInfo& config) override;

    TransportTargetRoomInfo
    GetTargetRoomInfo() const override;

    // ==================== 本地音视频发布 ====================

    bool StartPublishVideo() override;
    bool StopPublishVideo() override;

    bool StartPublishAudio() override;
    bool StopPublishAudio() override;

    /*
     * 这两个函数只把公共原始帧转交给RtcPushTransport。
     * 编码由RtcPushTransport内部工作线程完成。
     */
    bool PushVideoFrame(
        const std::shared_ptr<I420Frame>& frame) override;

    bool PushAudioFrame(
        const std::shared_ptr<AudioFrame>& frame) override;

    bool IsPublishingVideo() const override;
    bool IsPublishingAudio() const override;

    // ==================== 远端用户订阅 ====================

    /**
     * @brief 订阅指定远端用户的音视频
     *
     * 每个远端用户创建独立的PeerConnection、Track和解码器。
     */
    bool SubscribeUserAV(
        const std::string& user_id) override;

    bool UnsubscribeUserAV(
        const std::string& user_id) override;

    bool IsUserSubscribedAV(
        const std::string& user_id) const override;

    std::vector<std::string>
    GetSubscribedUsers() const override;

    // ==================== 数据与状态回调 ====================

    void RegisterVideoCallback(
        VideoDataCallback callback) override;

    void RegisterAudioCallback(
        AudioDataCallback callback) override;

    void RegisterConnectionStateCallback(
        ConnectionStateCallback callback) override;

    bool IsInitialized() const override;

private:
    /**
     * @brief 处理本地推流PeerConnection产生的Offer
     *
     * 该函数由libdatachannel内部线程调用，只提取SDP并将
     * WHIP请求投递到信令线程，不直接执行HTTP请求。
     */
    void HandlePublishLocalDescription(
        const rtc::Description& description);

    /**
     * @brief 处理远端用户拉流PeerConnection产生的Offer
     */
    void HandlePullLocalDescription(
        const std::string& user_id,
        const rtc::Description& description);

    // 停止本地发布并删除WHIP服务端资源。
    void ClosePublish();

    // 关闭并删除全部远端用户拉流对象。
    void CloseAllPull();

    // ==================== 信令线程 ====================

    bool StartSignalingWorker();
    void StopSignalingWorker();

    /**
     * @brief 向信令线程投递一个HTTP信令任务
     *
     * 队列中只保存WHIP/WHEP控制任务，不保存音视频数据。
     */
    bool PostSignalingTask(
        std::function<void()> task);

    void SignalingWorkerLoop();

private:
    std::atomic<bool> m_initialized{false};

    /*
     * 使用shared_ptr是为了让PushVideoFrame等实时接口能够先取得
     * 一个安全副本，再释放状态锁后调用RtcPushTransport。
     */
    std::shared_ptr<RtcPushTransport> m_push_transport;

    std::unique_ptr<WhipWhepClient> m_signaling_client;

    PublishInfo m_publish_info;
    TransportTargetRoomInfo m_room_info;

    // 当前WHIP发布配置和服务端资源地址。
    PublishConfig m_publish_config;
    std::string m_publish_resource_url;
    std::uint64_t m_publish_generation{0};

    mutable std::mutex m_state_mutex;

    /*
     * 每个远端用户拥有独立的RtcPullTransport。
     */
    std::unordered_map<
        std::string,
        PullSession> m_pull_sessions;

    mutable std::mutex m_pull_mutex;

    /*
     * 上层注册的公共回调。
     * 拉流对象解码后，通过这些回调把公共帧转交给RenderEngine。
     */
    VideoDataCallback m_video_callback;
    AudioDataCallback m_audio_callback;
    ConnectionStateCallback m_connection_state_callback;

    mutable std::mutex m_callback_mutex;

    /*
     * Push PeerConnection的当前连接状态。
     * 拉流连接状态暂时由各自RtcPullTransport管理。
     */
    std::atomic<ConnectionState> m_connection_state{
        ConnectionState::kDisconnected
    };

    /*
     * WHIP/WHEP请求可能阻塞，因此统一由一个信令线程处理。
     * 该队列不会接触实时音视频帧。
     */
    std::deque<std::function<void()>> m_signaling_tasks;
    std::mutex m_signaling_mutex;
    std::condition_variable m_signaling_cv;
    std::thread m_signaling_worker;
    bool m_signaling_running{false};
};

} // namespace TRANSPORT