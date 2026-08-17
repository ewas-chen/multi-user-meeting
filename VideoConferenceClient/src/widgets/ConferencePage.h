#pragma once

#include "VceEngine.h"

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include <memory>
#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;

namespace VCE::CLIENT {

class VideoWidget;

/**
 * @brief 会议中的视频与媒体控制页面
 *
 * ConferencePage负责：
 * - 管理本地和远端VideoWidget；
 * - 显示会议及传输状态；
 * - 显示摄像头、麦克风和扬声器控件；
 * - 将用户操作转换为Qt信号交给MainWindow。
 *
 * 页面不会直接调用会议、采集或设备接口。VceEngine仅传递给
 * VideoWidget，用于在正确OpenGL上下文中完成用户渲染。
 */
class ConferencePage final : public QWidget {
    Q_OBJECT

public:
    explicit ConferencePage(
        std::shared_ptr<VCE::VceEngine> engine,
        QWidget* parent = nullptr);

    ~ConferencePage() override;

    ConferencePage(const ConferencePage&) = delete;
    ConferencePage& operator=(const ConferencePage&) = delete;
    ConferencePage(ConferencePage&&) = delete;
    ConferencePage& operator=(ConferencePage&&) = delete;

    /**
     * @brief 进入一个新会议并创建本地视频控件
     *
     * 调用前会清理上一次会议留下的视频控件。
     */
    bool enterMeeting(
        const QString& meeting_id,
        const QString& meeting_title,
        const QString& local_user_name,
        bool is_creator);

    /**
     * @brief 清理全部视频控件和会议界面状态
     *
     * 必须在VceEngine反初始化之前调用，保证VideoWidget析构时
     * 仍能在有效OpenGL上下文中执行RemoveUser。
     */
    void clearMeeting();

    [[nodiscard]]
    bool isMeetingActive() const noexcept {
        return m_meeting_active;
    }

    [[nodiscard]]
    const QString& meetingId() const noexcept {
        return m_meeting_id;
    }

    [[nodiscard]]
    const QString& localUserName() const noexcept {
        return m_local_user_name;
    }

    [[nodiscard]]
    bool isCreator() const noexcept {
        return m_is_creator;
    }

    /**
     * @brief 增加服务端通知的新远端用户
     *
     * 空用户名、本地用户名和已经存在的用户会被忽略。
     */
    void addRemoteUsers(const QStringList& user_names);

    /**
     * @brief 删除已经离开会议的远端用户
     */
    void removeRemoteUsers(const QStringList& user_names);

    void setUserVideoEnabled(
        const QString& user_name,
        bool enable);

    void setUserAudioEnabled(
        const QString& user_name,
        bool enable);

    void setLocalCameraEnabled(bool enable);
    void setLocalMicrophoneEnabled(bool enable);

    /**
     * @brief 设置可用扬声器以及当前设备
     *
     * 只更新界面，不调用VceEngine设备接口。
     */
    void setSpeakerDevices(
        const std::vector<VCE::SpeakerDeviceInfo>& devices,
        const QString& current_speaker_id);

    void setTransportState(VCE::TransportState state);

    void setBusy(bool busy);

    [[nodiscard]]
    bool isBusy() const noexcept {
        return m_busy;
    }

    void showMessage(const QString& message, bool is_error);
    void clearMessage();

signals:
    void cameraToggleRequested(bool enable);
    void microphoneToggleRequested(bool enable);
    void speakerChangeRequested(const QString& speaker_id);

    void leaveMeetingRequested();
    void endMeetingRequested();

    /**
     * @brief 转发VideoWidget的渲染用户初始化错误
     */
    void renderInitializationFailed(
        const QString& user_name,
        int error_code);

private:
    struct UserTile final {
        QWidget* container{nullptr};
        VideoWidget* video_widget{nullptr};
        QLabel* status_label{nullptr};

        bool video_enabled{false};
        bool audio_enabled{false};
        bool is_local{false};
    };

    bool createUserTile(
        const QString& user_name,
        bool is_local);

    void removeUserTile(
        const QString& user_name);

    void rebuildVideoGrid();
    void updateUserTileStatus(
        const QString& user_name);

    void updateMediaButtonText();
    void updateMeetingHeader();

    void requestCameraToggle();
    void requestMicrophoneToggle();
    void requestSpeakerChange(int index);

private:
    std::shared_ptr<VCE::VceEngine> m_engine;

    QLabel* m_meeting_title_label{nullptr};
    QLabel* m_meeting_id_label{nullptr};
    QLabel* m_transport_state_label{nullptr};
    QLabel* m_message_label{nullptr};

    QScrollArea* m_video_scroll_area{nullptr};
    QWidget* m_video_container{nullptr};
    QGridLayout* m_video_grid{nullptr};

    QPushButton* m_camera_button{nullptr};
    QPushButton* m_microphone_button{nullptr};
    QComboBox* m_speaker_combo{nullptr};
    QPushButton* m_leave_button{nullptr};
    QPushButton* m_end_button{nullptr};

    QHash<QString, UserTile> m_user_tiles;
    QStringList m_user_order;

    QString m_meeting_id;
    QString m_meeting_title;
    QString m_local_user_name;

    bool m_meeting_active{false};
    bool m_is_creator{false};
    bool m_camera_enabled{false};
    bool m_microphone_enabled{false};
    bool m_busy{false};
};

} // namespace VCE::CLIENT