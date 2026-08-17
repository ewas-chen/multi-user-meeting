#include "VideoWidget.h"

#include <QtCore/QByteArray>
#include <QtCore/QDebug>
#include <QtGui/QHideEvent>
#include <QtGui/QOpenGLContext>
#include <QtGui/QShowEvent>
#include <QtWidgets/QSizePolicy>

#include <utility>

namespace {

std::string ToStdString(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return std::string(
        utf8.constData(),
        static_cast<std::size_t>(utf8.size()));
}

} // namespace

namespace VCE::CLIENT {

VideoWidget::VideoWidget(
    std::shared_ptr<VCE::VceEngine> engine,
    QString user_name,
    bool is_local,
    QWidget* parent)
    : QOpenGLWidget(parent),
      m_engine(std::move(engine)),
      m_user_name(std::move(user_name)),
      m_is_local(is_local) {
    setObjectName(QStringLiteral("VideoWidget_%1").arg(m_user_name));
    setAccessibleName(m_user_name);
    setMinimumSize(240, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    /*
     * RenderEngine每次会重新绘制完整视频帧，
     * 不需要QOpenGLWidget保留上一帧的局部更新区域。
     */
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    m_refresh_timer.setInterval(kRefreshIntervalMs);
    m_refresh_timer.setTimerType(Qt::PreciseTimer);

    connect(
        &m_refresh_timer,
        &QTimer::timeout,
        this,
        [this]() {
            update();
        });

    updateStatusToolTip();
}

VideoWidget::~VideoWidget() {
    m_refresh_timer.stop();
    removeRenderUser();
}

void VideoWidget::setVideoEnabled(bool enable) {
    if (m_video_enabled == enable) {
        return;
    }

    m_video_enabled = enable;
    updateStatusToolTip();
    update();
}

void VideoWidget::setAudioEnabled(bool enable) {
    if (m_audio_enabled == enable) {
        return;
    }

    /*
     * 这里只记录界面显示状态。
     * 远端音频仍由RenderEngine和AudioMixer统一处理。
     */
    m_audio_enabled = enable;
    updateStatusToolTip();
}

void VideoWidget::initializeGL() {
    initializeOpenGLFunctions();
    m_gl_initialized = true;
    clearFrame();

    if (m_render_user_added) {
        return;
    }

    if (!m_engine) {
        emit renderInitializationFailed(
            m_user_name,
            static_cast<int>(VCE::kRet_Invalid_Status));
        return;
    }

    if (m_user_name.isEmpty()) {
        emit renderInitializationFailed(
            m_user_name,
            static_cast<int>(VCE::kRet_InvalidParam));
        return;
    }

    /*
     * initializeGL执行期间，当前线程持有该QOpenGLWidget的上下文。
     * RenderEngine可以在这里安全创建Shader、纹理和网格资源。
     */
    const VCE::Result result = m_engine->AddUser(
        ToStdString(m_user_name),
        m_is_local);

    if (result != VCE::kRet_SUCCESS) {
        clearFrame();

        emit renderInitializationFailed(
            m_user_name,
            static_cast<int>(result));
        return;
    }

    m_render_user_added = true;

    /*
     * AddUser后立即同步一次当前控件尺寸，
     * 避免首次resizeGL发生在用户创建之前。
     */
    if (width() > 0 && height() > 0) {
        m_engine->UpdateUserVideoSize(
            ToStdString(m_user_name),
            width(),
            height());
    }
}

void VideoWidget::paintGL() {
    if (!m_gl_initialized ||
        !m_render_user_added ||
        !m_engine ||
        !m_video_enabled) {
        clearFrame();
        return;
    }

    const VCE::Result result =
        m_engine->RenderUser(
            ToStdString(m_user_name));

    /*
     * 暂时没有可渲染帧或用户已经失效时显示黑色背景。
     * 不在刷新循环中持续打印日志，避免每秒产生大量重复信息。
     */
    if (result != VCE::kRet_SUCCESS) {
        clearFrame();
    }
}

void VideoWidget::resizeGL(int width, int height) {
    if (!m_gl_initialized || width <= 0 || height <= 0) {
        return;
    }

    if (!m_render_user_added || !m_engine) {
        clearFrame();
        return;
    }

    const VCE::Result result =
        m_engine->UpdateUserVideoSize(
            ToStdString(m_user_name),
            width,
            height);

    if (result != VCE::kRet_SUCCESS) {
        clearFrame();
    }
}

void VideoWidget::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);

    if (!m_refresh_timer.isActive()) {
        m_refresh_timer.start();
    }
}

void VideoWidget::hideEvent(QHideEvent* event) {
    m_refresh_timer.stop();
    QOpenGLWidget::hideEvent(event);
}

void VideoWidget::clearFrame() noexcept {
    if (!m_gl_initialized) {
        return;
    }

    /*
     * RenderEngine使用I420纹理绘制视频。
     * 没有有效视频时只清理颜色缓冲，不修改底层用户状态。
     */
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.04F, 0.04F, 0.04F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
}

void VideoWidget::removeRenderUser() noexcept {
    if (!m_render_user_added) {
        return;
    }

    m_refresh_timer.stop();

    if (!m_engine) {
        m_render_user_added = false;
        return;
    }

    QOpenGLContext* gl_context = context();
    if (!gl_context || !gl_context->isValid()) {
        qWarning().noquote()
            << QStringLiteral(
                   "Cannot remove render user because OpenGL context "
                   "is unavailable: %1")
                   .arg(m_user_name);

        m_render_user_added = false;
        return;
    }

    /*
     * RemoveUser会释放该用户的OpenGL资源，
     * 必须在对应QOpenGLWidget上下文处于当前状态时调用。
     */
    makeCurrent();

    if (QOpenGLContext::currentContext() != gl_context) {
        qWarning().noquote()
            << QStringLiteral(
                   "Failed to activate OpenGL context for user: %1")
                   .arg(m_user_name);

        m_render_user_added = false;
        return;
    }

    const VCE::Result result =
        m_engine->RemoveUser(
            ToStdString(m_user_name));

    doneCurrent();
    m_render_user_added = false;

    if (result != VCE::kRet_SUCCESS) {
        qWarning().noquote()
            << QStringLiteral(
                   "Failed to remove render user %1, error=%2")
                   .arg(m_user_name)
                   .arg(static_cast<int>(result));
    }
}

void VideoWidget::updateStatusToolTip() {
    const QString role =
        m_is_local ? tr("Local user") : tr("Remote user");

    const QString video_state =
        m_video_enabled ? tr("enabled") : tr("disabled");

    const QString audio_state =
        m_audio_enabled ? tr("enabled") : tr("disabled");

    setToolTip(
        tr("%1 (%2)\nVideo: %3\nAudio: %4")
            .arg(m_user_name)
            .arg(role)
            .arg(video_state)
            .arg(audio_state));
}

} // namespace VCE::CLIENT