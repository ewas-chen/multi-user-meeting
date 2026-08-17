#pragma once

#include <string>

namespace VCE {

/**
 * @brief 业务服务事件回调接口
 *
 * ServiceManager通过该接口把服务端主动通知的会议事件
 * 传递给VceEngineImpl。
 *
 * 回调可能运行在RPC或网络线程中，不应直接操作Qt界面。
 */
class IServiceDataCallback
{
public:
    virtual ~IServiceDataCallback() = default;

    /**
     * @brief 新用户加入当前会议
     */
    virtual void OnUserJoined(
        const std::string& user_name) = 0;

    /**
     * @brief 用户离开当前会议
     *
     * 将源码中的OnUserLeaved修正为语义更准确的OnUserLeft。
     */
    virtual void OnUserLeft(
        const std::string& user_name) = 0;

    /**
     * @brief 当前会议已被结束
     */
    virtual void OnMeetingEnded() = 0;
};

} // namespace VCE