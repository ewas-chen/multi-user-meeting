#include "UserContext.h"

#include "GLFrameWork/VideoShader.h"
#include "VideoRender.h"
#include "utils/logManager.h"

#include <utility>

namespace RENDER {

UserContext::UserContext(
    std::string user_id,
    bool is_local)
    : m_user_id(std::move(user_id)),
      m_is_local(is_local)
{
}

UserContext::~UserContext() noexcept
{
    Uninitialize();
}

bool UserContext::InitWithShader(
    std::shared_ptr<VideoShader> shader,
    int video_width,
    int video_height)
{
    if (m_user_id.empty()) {
        LOG_ERROR(
            "Failed to initialize UserContext: "
            "user id is empty");
        return false;
    }

    if (video_width <= 0 ||
        video_height <= 0 ||
        (video_width % 2) != 0 ||
        (video_height % 2) != 0) {
        LOG_ERROR(
            "Failed to initialize UserContext for {}: "
            "invalid video size {}x{}",
            m_user_id,
            video_width,
            video_height);
        return false;
    }

    if (!shader || !shader->IsInitialized()) {
        LOG_ERROR(
            "Failed to initialize UserContext for {}: "
            "video shader is unavailable",
            m_user_id);
        return false;
    }

    if (m_initialized) {
        return UpdateVideoSize(
            video_width,
            video_height);
    }

    /*
     * 使用局部对象完成初始化。
     * 失败时不会修改现有VideoRender状态。
     */
    auto video_renderer =
        std::make_unique<VideoRender>(
            m_user_id,
            std::move(shader),
            m_is_local);

    if (!video_renderer->Init(
            video_width,
            video_height)) {
        LOG_ERROR(
            "Failed to initialize VideoRender "
            "for user: {}",
            m_user_id);
        return false;
    }

    m_video_renderer = std::move(video_renderer);
    m_av_sync_controller.Reset();
    m_initialized = true;

    /*
     * VideoRender初始化完成后才允许采集线程或
     * 网络接收线程向当前用户写入视频帧。
     */
    {
        std::lock_guard<std::mutex> lock(
            m_buffer_mutex);

        m_frame_buffer.clear();
        m_last_frame.reset();

        m_audio_clock_was_valid = false;
        m_accept_frames = true;
    }

    LOG_INFO(
        "UserContext initialized: user={}, local={}",
        m_user_id,
        m_is_local);

    return true;
}

void UserContext::Uninitialize() noexcept
{
    /*
     * 先停止接收新帧，再清空已有视频状态。
     * 正在执行的PushFrame会在释放互斥锁后被完整清理。
     */
    {
        std::lock_guard<std::mutex> lock(
            m_buffer_mutex);

        m_accept_frames = false;
        m_frame_buffer.clear();
        m_last_frame.reset();
        m_audio_clock_was_valid = false;
    }

    m_av_sync_controller.Reset();
    m_initialized = false;

    /*
     * VideoRender持有OpenGL资源，调用方必须保证
     * 当前线程仍持有对应的OpenGL上下文。
     */
    if (m_video_renderer) {
        m_video_renderer->Uninitialize();
        m_video_renderer.reset();
    }
}

bool UserContext::UpdateVideoSize(
    int width,
    int height) noexcept
{
    if (!m_initialized ||
        !m_video_renderer ||
        width <= 0 ||
        height <= 0) {
        return false;
    }

    return m_video_renderer->ResizeVideo(
        width,
        height);
}

bool UserContext::PushFrame(
    const std::shared_ptr<I420Frame>& frame)
{
    if (!frame || !frame->IsValid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(
        m_buffer_mutex);

    if (!m_accept_frames) {
        return false;
    }

    /*
     * map以timestamp_us为键，自动按照媒体时间排序。
     * 相同时间戳再次到达时使用新帧替换旧帧。
     */
    m_frame_buffer.insert_or_assign(
        frame->timestamp_us,
        frame);

    /*
     * 视频发生积压时删除时间戳最早的帧。
     * 本地用户仍会在RenderFrame中直接选择最新帧。
     */
    while (m_frame_buffer.size() >
           kMaxBufferedVideoFrames) {
        m_frame_buffer.erase(
            m_frame_buffer.begin());
    }

    return true;
}

void UserContext::HandleAudioClockTransitionLocked(
    bool audio_clock_valid)
{
    if (m_is_local ||
        audio_clock_valid ==
            m_audio_clock_was_valid) {
        return;
    }

    /*
     * 音频时钟失效或恢复后，之前建立的音视频偏移
     * 已经不能继续使用。
     */
    m_audio_clock_was_valid =
        audio_clock_valid;

    m_av_sync_controller.Reset();

    /*
     * 只保留时间戳最新的视频帧。
     *
     * 音频进入重新缓冲时，避免旧视频继续积压；
     * 音频时钟恢复时，使用最新视频帧建立新同步基准。
     */
    while (m_frame_buffer.size() > 1) {
        m_frame_buffer.erase(
            m_frame_buffer.begin());
    }

    LOG_INFO(
        "Remote A/V sync timeline reset: "
        "user={}, audio_clock={}",
        m_user_id,
        audio_clock_valid
            ? "available"
            : "unavailable");
}

bool UserContext::RenderFrame(
    std::optional<std::int64_t>
        audio_clock_us)
{
    if (!m_initialized ||
        !m_video_renderer) {
        return false;
    }

    std::shared_ptr<I420Frame> frame;

    /*
     * 无效的非正时间戳不能作为音频主时钟。
     */
    const bool audio_clock_valid =
        audio_clock_us.has_value() &&
        *audio_clock_us > 0;

    if (!audio_clock_valid) {
        audio_clock_us.reset();
    }

    /*
     * 锁内只执行状态更新、同步决策和shared_ptr选择。
     * OpenGL纹理上传与绘制在锁外进行。
     */
    {
        std::lock_guard<std::mutex> lock(
            m_buffer_mutex);

        if (m_is_local) {
            /*
             * 本地预览始终选择最新帧并清除积压，
             * 不参与远端音视频同步。
             */
            if (m_frame_buffer.empty()) {
                frame = m_last_frame;
            } else {
                frame =
                    m_frame_buffer.rbegin()->second;

                m_frame_buffer.clear();
            }
        } else {
            /*
             * 必须在检查视频帧之前处理时钟状态切换。
             * 即使当前没有视频帧，也要及时废弃旧同步基准。
             */
            HandleAudioClockTransitionLocked(
                audio_clock_valid);

            if (m_frame_buffer.empty()) {
                frame = m_last_frame;
            } else if (!audio_clock_valid) {
                /*
                 * 音频首次启动或重新缓冲期间没有可靠主时钟。
                 * 远端视频临时降级为显示最新帧，防止沿旧同步
                 * 队列逐帧播放并产生明显延迟。
                 */
                frame =
                    m_frame_buffer.rbegin()->second;

                m_frame_buffer.clear();
            } else {
                /*
                 * 音频时钟有效时，从最早的视频帧开始检查。
                 * 一次渲染调用可以连续丢弃多个过期帧，
                 * 直到找到可渲染帧、需要等待或队列为空。
                 */
                while (!m_frame_buffer.empty()) {
                    auto first_frame =
                        m_frame_buffer.begin();

                    const std::shared_ptr<I420Frame>
                        candidate =
                            first_frame->second;

                    if (!candidate) {
                        m_frame_buffer.erase(
                            first_frame);
                        continue;
                    }

                    const AVSyncDecision decision =
                        m_av_sync_controller.
                            EvaluateVideoFrame(
                                candidate->timestamp_us,
                                audio_clock_us);

                    /*
                     * 控制器检测到时间戳跳变时，当前帧作为
                     * 新时间线起点，并清除旧队列残留。
                     */
                    if (decision.timeline_reset) {
                        frame = candidate;
                        m_frame_buffer.clear();
                        break;
                    }

                    if (decision.action ==
                        AVSyncAction::kWait) {
                        /*
                         * 视频位于音频未来，保留队首帧等待
                         * 音频追上，并继续显示上一帧。
                         */
                        frame = m_last_frame;
                        break;
                    }

                    if (decision.action ==
                        AVSyncAction::kDrop) {
                        /*
                         * 视频明显落后于音频，丢弃当前帧并
                         * 继续检查下一帧。
                         */
                        m_frame_buffer.erase(
                            first_frame);
                        continue;
                    }

                    frame = candidate;

                    m_frame_buffer.erase(
                        first_frame);
                    break;
                }

                /*
                 * 如果本次把所有过期帧全部丢弃，
                 * 继续显示上一帧，等待后续视频到达。
                 */
                if (!frame) {
                    frame = m_last_frame;
                }
            }
        }

        if (frame) {
            m_last_frame = frame;
        }
    }

    /*
     * 尚未收到有效视频帧时显示黑色背景。
     * 这属于正常启动状态，不视为渲染错误。
     */
    if (!frame) {
        return m_video_renderer->ClearBuffer();
    }

    /*
     * OpenGL操作在互斥锁之外完成，避免GPU绘制期间
     * 阻塞采集线程或网络接收线程写入视频帧。
     */
    return m_video_renderer->RenderFrame(frame);
}

} // namespace RENDER