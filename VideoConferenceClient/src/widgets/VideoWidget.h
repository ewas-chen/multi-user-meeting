#pragma once

#include "VceEngine.h"

#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLFunctions>
#include <QtOpenGLWidgets/QOpenGLWidget>

#include <memory>

class QHideEvent;
class QShowEvent;
class QWidget;

namespace VCE::CLIENT {

/**
 * @brief 单个会议用户的OpenGL视频渲染控件
 *
 * 每个VideoWidget在构造时绑定一个固定用户，负责在正确的
 * QOpenGLWidget上下文中创建、渲染、调整和删除该用户的
 * RenderEngine资源。
 *
 * 用户名在构造后不可修改，避免initializeGL执行前后出现
 * 渲染用户不一致。
 */
class VideoWidget final
    : public QOpenGLWidget,
      protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit VideoWidget(
        std::shared_ptr<VCE::VceEngine> engine,
        QString user_name,
        bool is_local,
        QWidget* parent = nullptr);

    ~VideoWidget() override;

    VideoWidget(const VideoWidget&) = delete;
    VideoWidget& operator=(const VideoWidget&) = delete;
    VideoWidget(VideoWidget&&) = delete;
    VideoWidget& operator=(VideoWidget&&) = delete;

    [[nodiscard]]
    const QString& userName() const noexcept {
        return m_user_name;
    }

    [[nodiscard]]
    bool isLocalUser() const noexcept {
        return m_is_local;
    }

    [[nodiscard]]
    bool isRenderUserAdded() const noexcept {
        return m_render_user_added;
    }

    [[nodiscard]]
    bool isVideoEnabled() const noexcept {
        return m_video_enabled;
    }

    [[nodiscard]]
    bool isAudioEnabled() const noexcept {
        return m_audio_enabled;
    }

    /**
     * @brief 更新用户视频状态
     *
     * 视频关闭时停止调用RenderUser并显示黑色背景，
     * 但不删除该用户的RenderEngine资源。
     */
    void setVideoEnabled(bool enable);

    /**
     * @brief 更新用户音频状态
     *
     * 该状态只提供给Qt会议界面显示，不修改底层音频播放链路。
     */
    void setAudioEnabled(bool enable);

signals:
    /**
     * @brief AddUser执行失败
     *
     * error_code使用int传递，避免额外注册VCE::Result元类型。
     */
    void renderInitializationFailed(
        const QString& user_name,
        int error_code);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void clearFrame() noexcept;
    void removeRenderUser() noexcept;
    void updateStatusToolTip();

private:
    static constexpr int kTargetRenderFps{30};
    static constexpr int kRefreshIntervalMs{1000 / kTargetRenderFps};

    std::shared_ptr<VCE::VceEngine> m_engine;
    const QString m_user_name;
    const bool m_is_local{false};

    QTimer m_refresh_timer;

    bool m_gl_initialized{false};
    bool m_render_user_added{false};
    bool m_video_enabled{true};
    bool m_audio_enabled{true};
};

} // namespace VCE::CLIENT