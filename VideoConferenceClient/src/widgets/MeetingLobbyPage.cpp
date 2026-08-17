#include "MeetingLobbyPage.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

namespace {

QString ToQString(const std::string& value)
{
    return QString::fromUtf8(
        value.data(),
        static_cast<qsizetype>(value.size()));
}

} // namespace

namespace VCE::CLIENT {

MeetingLobbyPage::MeetingLobbyPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("meetingLobbyPage"));

    m_title_label = new QLabel(tr("Meeting Lobby"), this);
    m_title_label->setObjectName(QStringLiteral("lobbyTitleLabel"));

    m_current_user_label = new QLabel(tr("Not logged in"), this);
    m_current_user_label->setObjectName(
        QStringLiteral("currentUserLabel"));

    // ==================== 采集设备 ====================

    m_camera_combo = new QComboBox(this);
    m_camera_combo->setObjectName(
        QStringLiteral("cameraDeviceCombo"));

    m_microphone_combo = new QComboBox(this);
    m_microphone_combo->setObjectName(
        QStringLiteral("microphoneDeviceCombo"));

    auto* device_form_layout = new QFormLayout;
    device_form_layout->addRow(
        tr("Camera:"),
        m_camera_combo);

    device_form_layout->addRow(
        tr("Microphone:"),
        m_microphone_combo);

    auto* device_group =
        new QGroupBox(tr("Media Devices"), this);

    device_group->setLayout(device_form_layout);

    // ==================== 创建会议 ====================

    m_create_title_edit = new QLineEdit(this);
    m_create_title_edit->setObjectName(
        QStringLiteral("meetingTitleEdit"));

    m_create_title_edit->setPlaceholderText(
        tr("Enter meeting title"));

    m_create_description_edit = new QTextEdit(this);
    m_create_description_edit->setObjectName(
        QStringLiteral("meetingDescriptionEdit"));

    m_create_description_edit->setPlaceholderText(
        tr("Enter an optional meeting description"));

    m_create_description_edit->setMaximumHeight(100);

    m_create_button =
        new QPushButton(tr("Create Meeting"), this);

    m_create_button->setObjectName(
        QStringLiteral("createMeetingButton"));

    auto* create_form_layout = new QFormLayout;
    create_form_layout->addRow(
        tr("Title:"),
        m_create_title_edit);

    create_form_layout->addRow(
        tr("Description:"),
        m_create_description_edit);

    auto* create_layout = new QVBoxLayout;
    create_layout->addLayout(create_form_layout);
    create_layout->addWidget(
        m_create_button,
        0,
        Qt::AlignRight);

    auto* create_group =
        new QGroupBox(tr("Create Meeting"), this);

    create_group->setLayout(create_layout);

    // ==================== 加入会议 ====================

    m_join_meeting_id_edit = new QLineEdit(this);
    m_join_meeting_id_edit->setObjectName(
        QStringLiteral("joinMeetingIdEdit"));

    m_join_meeting_id_edit->setPlaceholderText(
        tr("Enter meeting ID"));

    m_join_button =
        new QPushButton(tr("Join Meeting"), this);

    m_join_button->setObjectName(
        QStringLiteral("joinMeetingButton"));

    auto* join_form_layout = new QFormLayout;
    join_form_layout->addRow(
        tr("Meeting ID:"),
        m_join_meeting_id_edit);

    auto* join_layout = new QVBoxLayout;
    join_layout->addLayout(join_form_layout);
    join_layout->addWidget(
        m_join_button,
        0,
        Qt::AlignRight);

    join_layout->addStretch();

    auto* join_group =
        new QGroupBox(tr("Join Meeting"), this);

    join_group->setLayout(join_layout);

    auto* meeting_actions_layout = new QHBoxLayout;
    meeting_actions_layout->setSpacing(12);
    meeting_actions_layout->addWidget(create_group, 1);
    meeting_actions_layout->addWidget(join_group, 1);

    // ==================== 页面布局 ====================

    m_message_label = new QLabel(this);
    m_message_label->setObjectName(
        QStringLiteral("lobbyMessageLabel"));

    m_message_label->setWordWrap(true);
    m_message_label->setVisible(false);

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(24, 24, 24, 24);
    root_layout->setSpacing(12);
    root_layout->addWidget(m_title_label);
    root_layout->addWidget(m_current_user_label);
    root_layout->addWidget(device_group);
    root_layout->addLayout(meeting_actions_layout);
    root_layout->addWidget(m_message_label);
    root_layout->addStretch();

    connect(
        m_create_button,
        &QPushButton::clicked,
        this,
        &MeetingLobbyPage::submitCreateMeeting);

    connect(
        m_join_button,
        &QPushButton::clicked,
        this,
        &MeetingLobbyPage::submitJoinMeeting);

    connect(
        m_create_title_edit,
        &QLineEdit::returnPressed,
        this,
        &MeetingLobbyPage::submitCreateMeeting);

    connect(
        m_join_meeting_id_edit,
        &QLineEdit::returnPressed,
        this,
        &MeetingLobbyPage::submitJoinMeeting);

    updateDeviceControls();
    setBusy(false);
}

void MeetingLobbyPage::setCurrentUser(
    const QString& user_name)
{
    m_current_user_name = user_name.trimmed();

    if (m_current_user_name.isEmpty()) {
        m_current_user_label->setText(
            tr("Not logged in"));
        return;
    }

    m_current_user_label->setText(
        tr("Signed in as: %1")
            .arg(m_current_user_name));
}

void MeetingLobbyPage::setCameraDevices(
    const std::vector<VCE::CameraDeviceInfo>& devices,
    const QString& current_device_id)
{
    const QSignalBlocker blocker(m_camera_combo);
    m_camera_combo->clear();

    int selected_index = -1;

    for (const VCE::CameraDeviceInfo& device : devices) {
        const QString device_id =
            ToQString(device.id).trimmed();

        QString display_name =
            ToQString(device.name).trimmed();

        if (device_id.isEmpty()) {
            continue;
        }

        if (display_name.isEmpty()) {
            display_name = device_id;
        }

        const int index = m_camera_combo->count();
        m_camera_combo->addItem(display_name, device_id);

        if (device_id == current_device_id) {
            selected_index = index;
        }
    }

    if (selected_index < 0 &&
        m_camera_combo->count() > 0) {
        selected_index = 0;
    }

    m_camera_combo->setCurrentIndex(selected_index);
    updateDeviceControls();
}

void MeetingLobbyPage::setMicrophoneDevices(
    const std::vector<VCE::MicDeviceInfo>& devices,
    const QString& current_device_id)
{
    const QSignalBlocker blocker(m_microphone_combo);
    m_microphone_combo->clear();

    int selected_index = -1;

    for (const VCE::MicDeviceInfo& device : devices) {
        const QString device_id =
            ToQString(device.id).trimmed();

        QString display_name =
            ToQString(device.name).trimmed();

        if (device_id.isEmpty()) {
            continue;
        }

        if (display_name.isEmpty()) {
            display_name = device_id;
        }

        const int index =
            m_microphone_combo->count();

        m_microphone_combo->addItem(
            display_name,
            device_id);

        if (device_id == current_device_id) {
            selected_index = index;
        }
    }

    if (selected_index < 0 &&
        m_microphone_combo->count() > 0) {
        selected_index = 0;
    }

    m_microphone_combo->setCurrentIndex(
        selected_index);

    updateDeviceControls();
}

QString MeetingLobbyPage::selectedCameraDeviceId() const
{
    if (m_camera_combo->currentIndex() < 0) {
        return {};
    }

    return m_camera_combo
        ->currentData()
        .toString()
        .trimmed();
}

QString MeetingLobbyPage::selectedMicrophoneDeviceId() const
{
    if (m_microphone_combo->currentIndex() < 0) {
        return {};
    }

    return m_microphone_combo
        ->currentData()
        .toString()
        .trimmed();
}

void MeetingLobbyPage::setBusy(bool busy)
{
    m_busy = busy;

    m_create_title_edit->setEnabled(!busy);
    m_create_description_edit->setEnabled(!busy);
    m_join_meeting_id_edit->setEnabled(!busy);

    m_create_button->setEnabled(!busy);
    m_join_button->setEnabled(!busy);

    updateDeviceControls();
}

void MeetingLobbyPage::showMessage(
    const QString& message,
    bool is_error)
{
    const QString normalized_message =
        message.trimmed();

    if (normalized_message.isEmpty()) {
        clearMessage();
        return;
    }

    m_message_label->setText(normalized_message);

    m_message_label->setStyleSheet(
        is_error
            ? QStringLiteral("color: #b3261e;")
            : QStringLiteral("color: #188038;"));

    m_message_label->setVisible(true);
}

void MeetingLobbyPage::clearMessage()
{
    m_message_label->clear();
    m_message_label->setVisible(false);
}

void MeetingLobbyPage::clearForms()
{
    m_create_title_edit->clear();
    m_create_description_edit->clear();
    m_join_meeting_id_edit->clear();
    clearMessage();
}

void MeetingLobbyPage::submitCreateMeeting()
{
    if (m_busy) {
        return;
    }

    const QString title =
        m_create_title_edit->text().trimmed();

    const QString description =
        m_create_description_edit
            ->toPlainText()
            .trimmed();

    if (title.isEmpty()) {
        showMessage(
            tr("Meeting title cannot be empty."),
            true);

        m_create_title_edit->setFocus();
        return;
    }

    if (selectedCameraDeviceId().isEmpty()) {
        showMessage(
            tr("No camera device is selected."),
            true);

        return;
    }

    if (selectedMicrophoneDeviceId().isEmpty()) {
        showMessage(
            tr("No microphone device is selected."),
            true);

        return;
    }

    clearMessage();
    setBusy(true);

    emit createMeetingRequested(
        title,
        description);
}

void MeetingLobbyPage::submitJoinMeeting()
{
    if (m_busy) {
        return;
    }

    const QString meeting_id =
        m_join_meeting_id_edit
            ->text()
            .trimmed();

    if (meeting_id.isEmpty()) {
        showMessage(
            tr("Meeting ID cannot be empty."),
            true);

        m_join_meeting_id_edit->setFocus();
        return;
    }

    if (selectedCameraDeviceId().isEmpty()) {
        showMessage(
            tr("No camera device is selected."),
            true);

        return;
    }

    if (selectedMicrophoneDeviceId().isEmpty()) {
        showMessage(
            tr("No microphone device is selected."),
            true);

        return;
    }

    clearMessage();
    setBusy(true);

    emit joinMeetingRequested(meeting_id);
}

void MeetingLobbyPage::updateDeviceControls()
{
    const bool camera_available =
        m_camera_combo->currentIndex() >= 0 &&
        !m_camera_combo
             ->currentData()
             .toString()
             .isEmpty();

    const bool microphone_available =
        m_microphone_combo->currentIndex() >= 0 &&
        !m_microphone_combo
             ->currentData()
             .toString()
             .isEmpty();

    m_camera_combo->setEnabled(
        !m_busy && camera_available);

    m_microphone_combo->setEnabled(
        !m_busy && microphone_available);
}

} // namespace VCE::CLIENT