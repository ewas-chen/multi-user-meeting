#pragma once

#include "VceTypes.h"

#include <memory>
#include <string>

namespace VCE {

/**
 * @brief 传输模块数据和状态回调接口
 *
 * TransportManager通过该接口把解码后的远端音视频帧
 * 以及RTC连接状态传递给VceEngineImpl。
 *
 * 音视频回调通常运行在传输模块的解码线程中，实现中应尽快
 * 将帧转交给RenderManager，避免阻塞后续RTC数据处理。
 */
class ITransportDataCallback
{
public:
    virtual ~ITransportDataCallback() = default;

    /**
     * @brief 接收远端用户的I420视频帧
     *
     * @param user_id 远端用户ID
     * @param capture_type 视频来源类型，当前主要为摄像头
     * @param frame 解码后的I420视频帧
     */
    virtual void OnTransportVideoFrame(
        const std::string& user_id,
        CaptureType capture_type,
        const std::shared_ptr<I420Frame>& frame) = 0;

    /**
     * @brief 接收远端用户的Float32 PCM音频帧
     */
    virtual void OnTransportAudioFrame(
        const std::string& user_id,
        const std::shared_ptr<AudioFrame>& frame) = 0;

    /**
     * @brief RTC传输连接状态发生变化
     */
    virtual void OnTransportConnectionStateChanged(
        TransportState state) = 0;
};

} // namespace VCE