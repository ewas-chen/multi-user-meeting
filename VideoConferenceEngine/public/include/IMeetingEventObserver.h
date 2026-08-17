#pragma once

#include "VceGlobal.h"
#include "VceTypes.h"

#include <string>
#include <vector>

namespace VCE {

/**
 * @brief 会议事件观察者
 *
 * 客户端通过实现该接口接收用户上下线、媒体状态和传输状态事件。
 *
 * 注意：这些回调可能来自业务服务线程、RTC线程或媒体状态检测线程。
 * 如果回调中需要更新Qt等界面，必须将操作投递到UI线程执行。
 */
class VCE_API IMeetingEventObserver
{
public:
    virtual ~IMeetingEventObserver() = default;

    /**
     * @brief 用户加入会议
     *
     * 加入已有会议时可能一次返回多个用户；
     * 会议进行期间通常只包含一个新加入用户。
     */
    virtual void OnUserJoined(
        const std::vector<std::string>& user_names) = 0;

    /**
     * @brief 用户离开会议
     */
    virtual void OnUserLeft(
        const std::vector<std::string>& user_names) = 0;

    /**
     * @brief 当前会议已结束
     */
    virtual void OnMeetingEnded() = 0;

    /**
     * @brief 远端用户的视频发送状态发生变化
     *
     * enable为true表示已经收到该用户的视频帧；
     * enable为false表示用户停止视频或长时间未收到视频帧。
     */
    virtual void OnUserVideoEnable(
        const std::string& user_name,
        bool enable) = 0;

    /**
     * @brief 远端用户的音频发送状态发生变化
     *
     * enable为true表示已经收到该用户的音频帧；
     * enable为false表示用户停止音频或长时间未收到音频帧。
     */
    virtual void OnUserAudioEnable(
        const std::string& user_name,
        bool enable) = 0;

    /**
     * @brief RTC传输连接状态发生变化
     *
     * 相比源码只通知connected布尔值，这里保留完整状态，
     * 便于客户端区分正在连接、连接失败、断开和关闭。
     */
    virtual void OnTransportConnectionStateChanged(
        TransportState state) = 0;
};

} // namespace VCE