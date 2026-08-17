#pragma once

#include "VceTypes.h"

#include <memory>

namespace VCE {

/**
 * @brief 采集模块数据回调接口
 *
 * CaptureManager通过该接口将采集到的原始音视频帧 
 * 传递给VceEngineImpl。
 *
 * 回调可能运行在OBS采集线程中，具体实现应尽快完成数据转发，
 * 不应在回调中进行编码、文件写入或其他耗时操作。
 */
class ICaptureDataCallback
{
public:
    virtual ~ICaptureDataCallback() = default;

    /**
     * @brief 接收摄像头采集的I420视频帧
     */
    virtual void OnCaptureVideoFrame(const std::shared_ptr<I420Frame>& frame) = 0;
        
    /**
     * @brief 接收麦克风采集的Float32 PCM音频帧
     */
    virtual void OnCaptureAudioFrame(const std::shared_ptr<AudioFrame>& frame) = 0;
};

} // namespace VCE