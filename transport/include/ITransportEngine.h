#pragma once

#include "TransportDefine.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TRANSPORT {

/**
 * 接收到远端视频帧时的回调
 *
 * user_id用于标识视频所属远端用户。
 * frame直接使用CAPTURE::I420Frame，无需类型转换。
 *
 * 回调可能由传输模块工作线程触发，回调内部不应执行耗时操作。
 */
using VideoDataCallback = std::function<void(
    const std::string& user_id,
    const std::shared_ptr<I420Frame>& frame)>;

/**
 * 接收到远端音频帧时的回调
 *
 * frame中的音频格式为交错存储的Float32 PCM。
 */
using AudioDataCallback = std::function<void(
    const std::string& user_id,
    const std::shared_ptr<AudioFrame>& frame)>;

/**
 * WebRTC连接状态变化回调
 */
using ConnectionStateCallback =
    std::function<void(ConnectionState state)>;

/**
 * 音视频传输引擎公共接口
 *
 * 主要职责：
 * 1. 编码并发布本地采集到的音视频帧。
 * 2. 订阅远端用户并解码收到的音视频帧。
 * 3. 管理WHIP/WHEP和WebRTC连接。
 *
 * 推荐调用顺序：
 * Create()
 * -> Initialize()
 * -> SetTargetRoomInfo()
 * -> StartPublish...()/SubscribeUserAV()
 * -> Push...Frame()
 * -> Uninit()
 */
class TRANSPORT_ENGINE_API ITransportEngine
{
public:
    virtual ~ITransportEngine() = default;

    ITransportEngine(const ITransportEngine&) = delete;
    ITransportEngine& operator=(const ITransportEngine&) = delete;
    ITransportEngine(ITransportEngine&&) = delete;
    ITransportEngine& operator=(ITransportEngine&&) = delete;

    /**
     * 创建传输引擎实例
     */
    static std::unique_ptr<ITransportEngine> Create();

    /**
     * 初始化传输模块和音视频编解码器
     *
     * publish_info应与CaptureEngine的音视频输出配置一致。
     */
    virtual bool Initialize(
        const PublishInfo& publish_info) = 0;

    /**
     * 停止所有发布和订阅并释放资源
     *
     * 允许在未初始化或已经反初始化的状态下调用。
     */
    virtual void Uninit() = 0;

    /**
     * 设置目标服务器和会议房间信息
     *
     * 应在开始发布或订阅前调用。
     */
    virtual void SetTargetRoomInfo(const TransportTargetRoomInfo& room_info) = 0;

    /**
     * 获取当前目标房间信息
     */
    virtual TransportTargetRoomInfo GetTargetRoomInfo() const = 0;

    // 本地音视频发布

    /**
     * 创建视频发送轨道并开始发布本地视频
     */
    virtual bool StartPublishVideo() = 0;

    /**
     * 停止发布本地视频
     */
    virtual bool StopPublishVideo() = 0;

    /**
     * 创建音频发送轨道并开始发布本地音频
     */
    virtual bool StartPublishAudio() = 0;

    /**
     * 停止发布本地音频
     */
    virtual bool StopPublishAudio() = 0;

    /**
     * 提交一帧本地I420视频
     *
     * 实现层应将帧放入有界编码队列，不应阻塞OBS采集回调。
     */
    virtual bool PushVideoFrame(const std::shared_ptr<I420Frame>& frame) = 0;
        
    /**
     * 提交一帧本地Float32 PCM音频
     *
     * 实现层负责将音频整理为Opus需要的固定帧长。
     */
    virtual bool PushAudioFrame(const std::shared_ptr<AudioFrame>& frame) = 0;

    virtual bool IsPublishingVideo() const = 0;
    virtual bool IsPublishingAudio() const = 0;

    // 远端用户订阅

    /**
     * 订阅指定远端用户的音频和视频
     */
    virtual bool SubscribeUserAV(
        const std::string& user_id) = 0;

    /**
     * 取消订阅指定远端用户
     */
    virtual bool UnsubscribeUserAV(
        const std::string& user_id) = 0;

    /**
     * 查询指定用户是否已经订阅
     */
    virtual bool IsUserSubscribedAV(
        const std::string& user_id) const = 0;

    /**
     * 获取当前已经订阅的远端用户ID
     */
    virtual std::vector<std::string>
    GetSubscribedUsers() const = 0;

    // 回调注册

    /**
     * 注册远端视频帧回调
     *
     * 传入空std::function表示取消注册。
     */
    virtual void RegisterVideoCallback(VideoDataCallback callback) = 0;

    /**
     * 注册远端音频帧回调
     *
     * 传入空std::function表示取消注册。
     */
    virtual void RegisterAudioCallback(AudioDataCallback callback) = 0;

    /**
     * 注册WebRTC连接状态回调
     *
     * 传入空std::function表示取消注册。
     */
    virtual void RegisterConnectionStateCallback(ConnectionStateCallback callback) = 0;
        
    /**
     * 查询传输引擎是否已经初始化
     */
    virtual bool IsInitialized() const = 0;

protected:
    ITransportEngine() = default;
};

} // namespace TRANSPORT