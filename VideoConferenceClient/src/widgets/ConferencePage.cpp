#include "ConferencePage.h"
#include "VideoWidget.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QVariant>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

namespace {

QString ToQString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

namespace VCE::CLIENT {

ConferencePage::ConferencePage(std::shared_ptr<VCE::VceEngine> engine, QWidget* parent)
    : QWidget(parent),
      m_engine(std::move(engine))
{
    setObjectName(QStringLiteral("conferencePage"));

    m_meeting_title_label = new QLabel(tr("Video Conference"), this);
    m_meeting_title_label->setObjectName(QStringLiteral("meetingTitleLabel"));

    m_meeting_id_label = new QLabel(tr("Not in a meeting"), this);
    m_meeting_id_label->setObjectName(QStringLiteral("meetingIdLabel"));
    m_meeting_id_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_transport_state_label = new QLabel(this);
    m_transport_state_label->setObjectName(QStringLiteral("transportStateLabel"));
    m_transport_state_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* meeting_info_layout = new QVBoxLayout;
    meeting_info_layout->setContentsMargins(0, 0, 0, 0);
    meeting_info_layout->setSpacing(2);
    meeting_info_layout->addWidget(m_meeting_title_label);
    meeting_info_layout->addWidget(m_meeting_id_label);

    auto* header_layout = new QHBoxLayout;
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->addLayout(meeting_info_layout);
    header_layout->addStretch();
    header_layout->addWidget(m_transport_state_label);

    m_video_container = new QWidget(this);
    m_video_container->setObjectName(QStringLiteral("videoContainer"));
    m_video_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_video_grid = new QGridLayout(m_video_container);
    m_video_grid->setContentsMargins(8, 8, 8, 8);
    m_video_grid->setHorizontalSpacing(8);
    m_video_grid->setVerticalSpacing(8);

    m_video_scroll_area = new QScrollArea(this);
    m_video_scroll_area->setObjectName(QStringLiteral("videoScrollArea"));
    m_video_scroll_area->setWidgetResizable(true);
    m_video_scroll_area->setFrameShape(QFrame::NoFrame);
    m_video_scroll_area->setAlignment(Qt::AlignCenter);
    m_video_scroll_area->setMinimumHeight(320);
    m_video_scroll_area->setWidget(m_video_container);

    m_camera_button = new QPushButton(this);
    m_camera_button->setObjectName(QStringLiteral("cameraButton"));

    m_microphone_button = new QPushButton(this);
    m_microphone_button->setObjectName(QStringLiteral("microphoneButton"));

    m_speaker_combo = new QComboBox(this);
    m_speaker_combo->setObjectName(QStringLiteral("speakerCombo"));
    m_speaker_combo->setMinimumWidth(220);
    m_speaker_combo->setToolTip(tr("Audio output device"));

    m_leave_button = new QPushButton(tr("Leave Meeting"), this);
    m_leave_button->setObjectName(QStringLiteral("leaveMeetingButton"));

    m_end_button = new QPushButton(tr("End Meeting"), this);
    m_end_button->setObjectName(QStringLiteral("endMeetingButton"));
    m_end_button->setVisible(false);

    auto* controls_layout = new QHBoxLayout;
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(8);
    controls_layout->addWidget(m_camera_button);
    controls_layout->addWidget(m_microphone_button);
    controls_layout->addWidget(m_speaker_combo);
    controls_layout->addStretch();
    controls_layout->addWidget(m_leave_button);
    controls_layout->addWidget(m_end_button);

    m_message_label = new QLabel(this);
    m_message_label->setObjectName(QStringLiteral("conferenceMessageLabel"));
    m_message_label->setWordWrap(true);
    m_message_label->setVisible(false);

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(16, 16, 16, 16);
    root_layout->setSpacing(12);
    root_layout->addLayout(header_layout);
    root_layout->addWidget(m_video_scroll_area, 1);
    root_layout->addWidget(m_message_label);
    root_layout->addLayout(controls_layout);

    connect(m_camera_button, &QPushButton::clicked,
            this, &ConferencePage::requestCameraToggle);

    connect(m_microphone_button, &QPushButton::clicked,
            this, &ConferencePage::requestMicrophoneToggle);

    connect(m_speaker_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConferencePage::requestSpeakerChange);

    connect(m_leave_button, &QPushButton::clicked, this, [this]() {
        if (!m_meeting_active || m_busy) {
            return;
        }

        setBusy(true);
        emit leaveMeetingRequested();
    });

    connect(m_end_button, &QPushButton::clicked, this, [this]() {
        if (!m_meeting_active || !m_is_creator || m_busy) {
            return;
        }

        setBusy(true);
        emit endMeetingRequested();
    });

    updateMeetingHeader();
    updateMediaButtonText();
    setTransportState(VCE::TransportState::kDisconnected);
    setBusy(false);
}

ConferencePage::~ConferencePage()
{
    /*
     * VideoWidget需要在有效OpenGL上下文中删除渲染用户。
     * 因此在VceEngine成员释放前主动销毁所有视频窗口。
     */
    clearMeeting();
}

bool ConferencePage::enterMeeting(const QString& meeting_id,
                                  const QString& meeting_title,
                                  const QString& local_user_name,
                                  bool is_creator)
{
    const QString normalized_meeting_id = meeting_id.trimmed();
    const QString normalized_user_name = local_user_name.trimmed();

    if (!m_engine) {
        showMessage(tr("The meeting engine is unavailable."), true);
        return false;
    }

    if (normalized_meeting_id.isEmpty() || normalized_user_name.isEmpty()) {
        showMessage(tr("Meeting ID and local user name cannot be empty."), true);
        return false;
    }

    clearMeeting();

    m_meeting_id = normalized_meeting_id;
    m_meeting_title = meeting_title.trimmed();
    m_local_user_name = normalized_user_name;
    m_is_creator = is_creator;
    m_meeting_active = true;
    m_camera_enabled = false;
    m_microphone_enabled = false;

    if (!createUserTile(m_local_user_name, true)) {
        clearMeeting();
        showMessage(tr("Failed to create the local video window."), true);
        return false;
    }

    updateMeetingHeader();
    updateMediaButtonText();
    m_end_button->setVisible(m_is_creator);
    setTransportState(VCE::TransportState::kConnecting);
    setBusy(false);
    clearMessage();
    return true;
}

void ConferencePage::clearMeeting()
{
    m_meeting_active = false;
    m_busy = false;

    /*
     * 先从布局中移除项目，再直接销毁容器。
     * 容器会同步销毁VideoWidget，并在其OpenGL上下文中移除渲染用户。
     */
    while (QLayoutItem* item = m_video_grid->takeAt(0)) {
        delete item;
    }

    for (auto it = m_user_tiles.begin(); it != m_user_tiles.end(); ++it) {
        delete it.value().container;
    }

    m_user_tiles.clear();
    m_user_order.clear();

    {
        const QSignalBlocker blocker(m_speaker_combo);
        m_speaker_combo->clear();
    }

    m_meeting_id.clear();
    m_meeting_title.clear();
    m_local_user_name.clear();
    m_is_creator = false;
    m_camera_enabled = false;
    m_microphone_enabled = false;

    m_end_button->setVisible(false);
    updateMeetingHeader();
    updateMediaButtonText();
    setTransportState(VCE::TransportState::kDisconnected);
    clearMessage();
    setBusy(false);
}

void ConferencePage::addRemoteUsers(const QStringList& user_names)
{
    if (!m_meeting_active) {
        return;
    }

    for (const QString& value : user_names) {
        const QString user_name = value.trimmed();

        if (user_name.isEmpty() || user_name == m_local_user_name ||
            m_user_tiles.contains(user_name)) {
            continue;
        }

        createUserTile(user_name, false);
    }
}

void ConferencePage::removeRemoteUsers(const QStringList& user_names)
{
    for (const QString& value : user_names) {
        const QString user_name = value.trimmed();

        if (user_name.isEmpty() || user_name == m_local_user_name) {
            continue;
        }

        removeUserTile(user_name);
    }
}

void ConferencePage::setUserVideoEnabled(const QString& user_name, bool enable)
{
    QString normalized_user_name = user_name.trimmed();
    auto it = m_user_tiles.find(normalized_user_name);

    /*
     * RTC媒体事件可能略早于会议成员事件。
     * 第一次收到有效视频时允许补建远端用户窗口。
     */
    if (it == m_user_tiles.end() && enable && m_meeting_active &&
        !normalized_user_name.isEmpty() &&
        normalized_user_name != m_local_user_name) {
        createUserTile(normalized_user_name, false);
        it = m_user_tiles.find(normalized_user_name);
    }

    if (it == m_user_tiles.end()) {
        return;
    }

    it.value().video_enabled = enable;

    if (it.value().video_widget) {
        it.value().video_widget->setVideoEnabled(enable);
    }

    updateUserTileStatus(normalized_user_name);
}

void ConferencePage::setUserAudioEnabled(const QString& user_name, bool enable)
{
    QString normalized_user_name = user_name.trimmed();
    auto it = m_user_tiles.find(normalized_user_name);

    if (it == m_user_tiles.end() && enable && m_meeting_active &&
        !normalized_user_name.isEmpty() &&
        normalized_user_name != m_local_user_name) {
        createUserTile(normalized_user_name, false);
        it = m_user_tiles.find(normalized_user_name);
    }

    if (it == m_user_tiles.end()) {
        return;
    }

    it.value().audio_enabled = enable;

    if (it.value().video_widget) {
        it.value().video_widget->setAudioEnabled(enable);
    }

    updateUserTileStatus(normalized_user_name);
}

void ConferencePage::setLocalCameraEnabled(bool enable)
{
    if (!m_meeting_active) {
        return;
    }

    m_camera_enabled = enable;
    setUserVideoEnabled(m_local_user_name, enable);
    updateMediaButtonText();
}

void ConferencePage::setLocalMicrophoneEnabled(bool enable)
{
    if (!m_meeting_active) {
        return;
    }

    m_microphone_enabled = enable;
    setUserAudioEnabled(m_local_user_name, enable);
    updateMediaButtonText();
}

void ConferencePage::setSpeakerDevices(
    const std::vector<VCE::SpeakerDeviceInfo>& devices,
    const QString& current_speaker_id)
{
    const QSignalBlocker blocker(m_speaker_combo);
    m_speaker_combo->clear();

    int current_index = -1;
    int default_index = -1;

    for (const VCE::SpeakerDeviceInfo& device : devices) {
        const QString device_id = ToQString(device.id);
        QString display_name = ToQString(device.name);

        if (device_id.isEmpty()) {
            continue;
        }

        if (display_name.isEmpty()) {
            display_name = device_id;
        }

        if (device.is_default) {
            display_name += tr(" (Default)");
        }

        const int index = m_speaker_combo->count();
        m_speaker_combo->addItem(display_name, device_id);

        if (device_id == current_speaker_id) {
            current_index = index;
        }

        if (device.is_default && default_index < 0) {
            default_index = index;
        }
    }

    if (current_index < 0) {
        current_index = default_index;
    }

    m_speaker_combo->setCurrentIndex(current_index);
    setBusy(m_busy);
}

void ConferencePage::setTransportState(VCE::TransportState state)
{
    QString state_text;
    QString state_color;

    switch (state) {
    case VCE::TransportState::kDisconnected:
        state_text = tr("Disconnected");
        state_color = QStringLiteral("#777777");
        break;

    case VCE::TransportState::kConnecting:
        state_text = tr("Connecting");
        state_color = QStringLiteral("#b26a00");
        break;

    case VCE::TransportState::kConnected:
        state_text = tr("Connected");
        state_color = QStringLiteral("#188038");
        break;

    case VCE::TransportState::kReconnecting:
        state_text = tr("Reconnecting");
        state_color = QStringLiteral("#b26a00");
        break;

    case VCE::TransportState::kDisconnecting:
        state_text = tr("Disconnecting");
        state_color = QStringLiteral("#777777");
        break;

    case VCE::TransportState::kFailed:
        state_text = tr("Connection Failed");
        state_color = QStringLiteral("#b3261e");
        break;

    case VCE::TransportState::kClosed:
        state_text = tr("Closed");
        state_color = QStringLiteral("#777777");
        break;
    }

    m_transport_state_label->setText(state_text);
    m_transport_state_label->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(state_color));
}

void ConferencePage::setBusy(bool busy)
{
    m_busy = busy;

    const bool controls_enabled = m_meeting_active && !m_busy;
    const bool speaker_available =
        m_speaker_combo->currentIndex() >= 0 &&
        !m_speaker_combo->currentData().toString().isEmpty();

    m_camera_button->setEnabled(controls_enabled);
    m_microphone_button->setEnabled(controls_enabled);
    m_speaker_combo->setEnabled(controls_enabled && speaker_available);
    m_leave_button->setEnabled(controls_enabled);
    m_end_button->setEnabled(controls_enabled && m_is_creator);
    m_end_button->setVisible(m_meeting_active && m_is_creator);
}

void ConferencePage::showMessage(const QString& message, bool is_error)
{
    const QString normalized_message = message.trimmed();

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

void ConferencePage::clearMessage()
{
    m_message_label->clear();
    m_message_label->setVisible(false);
}

bool ConferencePage::createUserTile(const QString& user_name, bool is_local)
{
    const QString normalized_user_name = user_name.trimmed();

    if (!m_engine || normalized_user_name.isEmpty() ||
        m_user_tiles.contains(normalized_user_name)) {
        return false;
    }

    auto* container = new QFrame(m_video_container);
    container->setObjectName(QStringLiteral("userVideoTile"));
    container->setFrameShape(QFrame::StyledPanel);
    container->setMinimumSize(260, 220);
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* video_widget =
        new VideoWidget(m_engine, normalized_user_name, is_local, container);

    auto* status_label = new QLabel(container);
    status_label->setObjectName(QStringLiteral("userStatusLabel"));
    status_label->setAlignment(Qt::AlignCenter);
    status_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* tile_layout = new QVBoxLayout(container);
    tile_layout->setContentsMargins(4, 4, 4, 4);
    tile_layout->setSpacing(4);
    tile_layout->addWidget(video_widget, 1);
    tile_layout->addWidget(status_label);

    connect(video_widget, &VideoWidget::renderInitializationFailed,
            this, &ConferencePage::renderInitializationFailed);

    UserTile tile;
    tile.container = container;
    tile.video_widget = video_widget;
    tile.status_label = status_label;
    tile.video_enabled = false;
    tile.audio_enabled = false;
    tile.is_local = is_local;

    video_widget->setVideoEnabled(false);
    video_widget->setAudioEnabled(false);

    m_user_tiles.insert(normalized_user_name, tile);
    m_user_order.append(normalized_user_name);

    updateUserTileStatus(normalized_user_name);
    rebuildVideoGrid();
    return true;
}

void ConferencePage::removeUserTile(const QString& user_name)
{
    auto it = m_user_tiles.find(user_name);

    if (it == m_user_tiles.end()) {
        return;
    }

    UserTile tile = it.value();
    m_user_tiles.erase(it);
    m_user_order.removeAll(user_name);

    if (tile.container) {
        m_video_grid->removeWidget(tile.container);
        delete tile.container;
    }

    rebuildVideoGrid();
}

void ConferencePage::rebuildVideoGrid()
{
    while (QLayoutItem* item = m_video_grid->takeAt(0)) {
        delete item;
    }

    const int user_count = m_user_order.size();
    const int column_count = user_count <= 1 ? 1 : (user_count <= 4 ? 2 : 3);

    for (int column = 0; column < 3; ++column) {
        m_video_grid->setColumnStretch(column, column < column_count ? 1 : 0);
    }

    for (int row = 0; row <= user_count; ++row) {
        m_video_grid->setRowStretch(row, 0);
    }

    int visible_index = 0;

    for (const QString& user_name : m_user_order) {
        const auto it = m_user_tiles.constFind(user_name);

        if (it == m_user_tiles.constEnd() || !it.value().container) {
            continue;
        }

        const int row = visible_index / column_count;
        const int column = visible_index % column_count;

        m_video_grid->addWidget(it.value().container, row, column);
        m_video_grid->setRowStretch(row, 1);
        it.value().container->show();
        ++visible_index;
    }
}

void ConferencePage::updateUserTileStatus(const QString& user_name)
{
    auto it = m_user_tiles.find(user_name);

    if (it == m_user_tiles.end() || !it.value().status_label) {
        return;
    }

    const QString user_type = it.value().is_local ? tr("Local") : tr("Remote");
    const QString video_state =
        it.value().video_enabled ? tr("Video On") : tr("Video Off");
    const QString audio_state =
        it.value().audio_enabled ? tr("Audio On") : tr("Audio Off");

    it.value().status_label->setText(
        tr("%1 | %2 | %3 | %4")
            .arg(user_name)
            .arg(user_type)
            .arg(video_state)
            .arg(audio_state));
}

void ConferencePage::updateMediaButtonText()
{
    m_camera_button->setText(
        m_camera_enabled ? tr("Stop Camera") : tr("Start Camera"));

    m_microphone_button->setText(
        m_microphone_enabled ? tr("Mute Microphone")
                             : tr("Unmute Microphone"));
}

void ConferencePage::updateMeetingHeader()
{
    if (!m_meeting_active) {
        m_meeting_title_label->setText(tr("Video Conference"));
        m_meeting_id_label->setText(tr("Not in a meeting"));
        return;
    }

    const QString title =
        m_meeting_title.isEmpty() ? tr("Video Conference") : m_meeting_title;

    m_meeting_title_label->setText(title);
    m_meeting_id_label->setText(
        tr("Meeting ID: %1 | User: %2")
            .arg(m_meeting_id)
            .arg(m_local_user_name));
}

void ConferencePage::requestCameraToggle()
{
    if (!m_meeting_active || m_busy) {
        return;
    }

    setBusy(true);
    emit cameraToggleRequested(!m_camera_enabled);
}

void ConferencePage::requestMicrophoneToggle()
{
    if (!m_meeting_active || m_busy) {
        return;
    }

    setBusy(true);
    emit microphoneToggleRequested(!m_microphone_enabled);
}

void ConferencePage::requestSpeakerChange(int index)
{
    if (!m_meeting_active || m_busy || index < 0) {
        return;
    }

    const QString speaker_id =
        m_speaker_combo->itemData(index).toString().trimmed();

    if (speaker_id.isEmpty()) {
        return;
    }

    setBusy(true);
    emit speakerChangeRequested(speaker_id);
}

} // namespace VCE::CLIENT