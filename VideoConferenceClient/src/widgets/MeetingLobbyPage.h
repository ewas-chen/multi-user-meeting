#pragma once

#include "VceEngine.h"

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;

namespace VCE::CLIENT {

/**
 * @brief 登录后的会议大厅页面
 *
 * 页面只负责收集会议信息和采集设备选择，
 * 不直接调用VceEngine。
 */
class MeetingLobbyPage final : public QWidget {
    Q_OBJECT

public:
    explicit MeetingLobbyPage(QWidget* parent = nullptr);
    ~MeetingLobbyPage() override = default;

    MeetingLobbyPage(const MeetingLobbyPage&) = delete;
    MeetingLobbyPage& operator=(const MeetingLobbyPage&) = delete;
    MeetingLobbyPage(MeetingLobbyPage&&) = delete;
    MeetingLobbyPage& operator=(MeetingLobbyPage&&) = delete;

    void setCurrentUser(const QString& user_name);

    void setCameraDevices(
        const std::vector<VCE::CameraDeviceInfo>& devices,
        const QString& current_device_id = {});

    void setMicrophoneDevices(
        const std::vector<VCE::MicDeviceInfo>& devices,
        const QString& current_device_id = {});

    [[nodiscard]]
    QString selectedCameraDeviceId() const;

    [[nodiscard]]
    QString selectedMicrophoneDeviceId() const;

    void setBusy(bool busy);

    [[nodiscard]]
    bool isBusy() const noexcept {
        return m_busy;
    }

    void showMessage(const QString& message, bool is_error);
    void clearMessage();
    void clearForms();

signals:
    void createMeetingRequested(
        const QString& title,
        const QString& description);

    void joinMeetingRequested(
        const QString& meeting_id);

private:
    void submitCreateMeeting();
    void submitJoinMeeting();
    void updateDeviceControls();

private:
    QLabel* m_title_label{nullptr};
    QLabel* m_current_user_label{nullptr};
    QLabel* m_message_label{nullptr};

    QComboBox* m_camera_combo{nullptr};
    QComboBox* m_microphone_combo{nullptr};

    QLineEdit* m_create_title_edit{nullptr};
    QTextEdit* m_create_description_edit{nullptr};
    QPushButton* m_create_button{nullptr};

    QLineEdit* m_join_meeting_id_edit{nullptr};
    QPushButton* m_join_button{nullptr};

    QString m_current_user_name;
    bool m_busy{false};
};

} // namespace VCE::CLIENT