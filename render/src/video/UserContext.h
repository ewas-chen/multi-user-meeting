#pragma once

#include "RenderDefine.h"
#include "core/AVSyncController.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace RENDER {

class VideoRender;
class VideoShader;

/**
 * @brief 管理单个用户的视频渲染、帧缓冲和音视频同步状态
 *
 * 数据流：
 * 采集或网络线程
 * -> PushFrame()
 * -> 时间戳视频缓冲
 * -> RenderFrame()
 * -> AVSyncController
 * -> VideoRender
 *
 * 本地用户直接显示最新帧，优先保证预览低延迟。
 * 远端用户以音频播放时钟为主时钟，执行等待、渲染或丢帧。
 *
 * UserContext只保存shared_ptr<I420Frame>，不复制I420像素数据。
 * OpenGL资源必须在持有正确OpenGL上下文的渲染线程中操作。
 */
class RENDER_ENGINE_LOCAL UserContext final {
public:
    /**
     * @param user_id 用户唯一标识
     * @param is_local 是否为本地用户
     */
    UserContext(
        std::string user_id,
        bool is_local);

    ~UserContext() noexcept;

    UserContext(const UserContext&) = delete;
    UserContext& operator=(const UserContext&) = delete;
    UserContext(UserContext&&) = delete;
    UserContext& operator=(UserContext&&) = delete;

    /**
     * @brief 使用共享Shader创建当前用户的VideoRender
     *
     * 必须在持有正确OpenGL上下文的渲染线程调用。
     */
    bool InitWithShader(
        std::shared_ptr<VideoShader> shader,
        int video_width,
        int video_height);

    /**
     * @brief 停止接收视频帧并释放OpenGL资源
     *
     * 必须在持有正确OpenGL上下文的渲染线程调用。
     */
    void Uninitialize() noexcept;

    /**
     * @brief 修改视频显示区域尺寸
     *
     * 修改的是OpenGL Viewport，不是输入帧分辨率。
     */
    bool UpdateVideoSize(
        int width,
        int height) noexcept;

    /**
     * @brief 向当前用户的视频缓冲区写入一帧
     *
     * 可以从采集线程或网络接收线程调用。
     * 只保存shared_ptr，不复制I420像素数据。
     */
    bool PushFrame(
        const std::shared_ptr<I420Frame>& frame);

    /**
     * @brief 根据音频播放时钟选择视频帧并绘制
     *
     * @param audio_clock_us 当前音频设备播放到的公共媒体时间；
     *        音频时钟尚未建立或正在重新缓冲时传入std::nullopt。
     *
     * 本地用户：
     * - 不使用音频时钟；
     * - 直接选择最新视频帧。
     *
     * 远端用户：
     * - 视频超前时等待；
     * - 位于同步区间时渲染；
     * - 视频落后时丢帧追赶；
     * - 音频时钟失效时降级为显示最新视频帧；
     * - 音频时钟恢复时清理旧帧并重建同步基准。
     *
     * 必须在持有正确OpenGL上下文的渲染线程调用。
     */
    bool RenderFrame(
        std::optional<std::int64_t>
            audio_clock_us);

    [[nodiscard]]
    const std::string& GetUserId() const noexcept {
        return m_user_id;
    }

    [[nodiscard]]
    bool IsLocal() const noexcept {
        return m_is_local;
    }

    [[nodiscard]]
    bool IsInitialized() const noexcept {
        return m_initialized;
    }

private:
    /**
     * @brief 处理音频播放时钟的失效和恢复
     *
     * 音频时钟状态发生切换时：
     * - 重置AVSyncController旧同步基准；
     * - 丢弃积压视频帧，只保留最新一帧；
     * - 避免音频重新缓冲后继续使用旧偏移进行同步。
     *
     * 调用时必须持有m_buffer_mutex。
     */
    void HandleAudioClockTransitionLocked(
        bool audio_clock_valid);

private:
    /*
     * 远端保留约200～270ms视频帧，用于覆盖音频启动缓存
     * 和短时网络抖动。
     *
     * 本地预览始终选择最新帧，不会因此增加本地延迟。
     */
    static constexpr std::size_t
        kMaxBufferedVideoFrames{8};

    std::string m_user_id;
    bool m_is_local{false};

    /*
     * 只能由持有对应OpenGL上下文的渲染线程使用。
     * UserContext是VideoRender的唯一所有者。
     */
    std::unique_ptr<VideoRender> m_video_renderer;

    /*
     * 按timestamp_us自动排序：
     * begin()为最早帧，rbegin()为最新帧。
     */
    std::map<
        std::int64_t,
        std::shared_ptr<I420Frame>>
        m_frame_buffer;

    /*
     * 保护视频帧缓冲、上一帧、接收状态以及
     * 音频时钟状态切换信息。
     *
     * OpenGL绘制期间不会持有该锁。
     */
    mutable std::mutex m_buffer_mutex;

    // 缓冲区没有可渲染新帧时继续显示上一帧
    std::shared_ptr<I420Frame> m_last_frame;

    /*
     * 每个远端用户拥有独立同步控制器。
     * 本地预览不会使用该控制器。
     */
    AVSyncController m_av_sync_controller;

    /*
     * 上一次RenderFrame调用时音频主时钟是否有效。
     *
     * false -> true：
     * 音频首次启动或重新缓冲完成，需要建立新同步基准。
     *
     * true -> false：
     * 音频进入重新缓冲，需要立即废弃旧同步基准。
     *
     * 在m_buffer_mutex保护下访问。
     */
    bool m_audio_clock_was_valid{false};

    /*
     * Uninitialize开始后设为false，
     * 防止新帧进入即将销毁的用户对象。
     *
     * 在m_buffer_mutex保护下访问。
     */
    bool m_accept_frames{false};

    // 只由OpenGL渲染线程访问
    bool m_initialized{false};
};

} // namespace RENDER