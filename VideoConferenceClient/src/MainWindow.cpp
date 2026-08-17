#include "MainWindow.h"

#include "MeetingEventBridge.h"
#include "widgets/ConferencePage.h"
#include "widgets/LoginPage.h"
#include "widgets/MeetingLobbyPage.h"

#include <QtCore/QByteArray>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QStackedWidget>

#include <chrono>
#include <utility>
#include <vector>

namespace {

std::string ToStdString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

QString ToQString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::int64_t CurrentUnixTimeSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

namespace VCE::CLIENT {

MainWindow::MainWindow(VCE::EngineConfig engine_config, QWidget* parent)
    : QMainWindow(parent),
      m_engine_config(std::move(engine_config)),
      m_engine(VCE::VceEngine::GetInstance()),
      m_event_bridge(std::make_shared<MeetingEventBridge>())
{
    setupUi();
    connectPageSignals();
    connectMeetingEvents();
    showLoginPage();
}

MainWindow::~MainWindow()
{
    shutdown();
}

bool MainWindow::initialize()
{
    if (m_engine_initialized) {
        return true;
    }

    if (!m_engine) {
        m_login_page->showMessage(tr("Failed to create the meeting engine."), true);
        return false;
    }

    const VCE::Result result = m_engine->InitSubModel(m_engine_config);

    if (result != VCE::kRet_SUCCESS) {
        showOperationError(tr("Initialize meeting engine"), result);
        return false;
    }

    m_engine_initialized = true;

    if (m_event_bridge) {
        m_engine->AddObserver(m_event_bridge);
        m_observer_registered = true;
    }

    m_login_page->setBusy(false);
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    shutdown();
    event->accept();
}

void MainWindow::setupUi()
{
    setWindowTitle(tr("VCE Video Conference"));
    setMinimumSize(900, 600);
    resize(1200, 760);

    m_page_stack = new QStackedWidget(this);
    m_page_stack->setObjectName(QStringLiteral("mainPageStack"));

    m_login_page = new LoginPage(m_page_stack);
    m_lobby_page = new MeetingLobbyPage(m_page_stack);
    m_conference_page = new ConferencePage(m_engine, m_page_stack);

    m_page_stack->addWidget(m_login_page);
    m_page_stack->addWidget(m_lobby_page);
    m_page_stack->addWidget(m_conference_page);

    setCentralWidget(m_page_stack);
}

void MainWindow::connectPageSignals()
{
    connect(m_login_page, &LoginPage::loginRequested,
            this, &MainWindow::handleLoginRequested);

    connect(m_login_page, &LoginPage::registerRequested,
            this, &MainWindow::handleRegisterRequested);

    connect(m_lobby_page, &MeetingLobbyPage::createMeetingRequested,
            this, &MainWindow::handleCreateMeetingRequested);

    connect(m_lobby_page, &MeetingLobbyPage::joinMeetingRequested,
            this, &MainWindow::handleJoinMeetingRequested);

    connect(m_conference_page, &ConferencePage::cameraToggleRequested,
            this, &MainWindow::handleCameraToggleRequested);

    connect(m_conference_page, &ConferencePage::microphoneToggleRequested,
            this, &MainWindow::handleMicrophoneToggleRequested);

    connect(m_conference_page, &ConferencePage::speakerChangeRequested,
            this, &MainWindow::handleSpeakerChangeRequested);

    connect(m_conference_page, &ConferencePage::leaveMeetingRequested,
            this, &MainWindow::handleLeaveMeetingRequested);

    connect(m_conference_page, &ConferencePage::endMeetingRequested,
            this, &MainWindow::handleEndMeetingRequested);

    connect(m_conference_page, &ConferencePage::renderInitializationFailed,
            this, &MainWindow::handleRenderInitializationFailed);
}

void MainWindow::connectMeetingEvents()
{
    if (!m_event_bridge) {
        return;
    }

    connect(m_event_bridge.get(), &MeetingEventBridge::usersJoined,
            this, &MainWindow::handleUsersJoined);

    connect(m_event_bridge.get(), &MeetingEventBridge::usersLeft,
            this, &MainWindow::handleUsersLeft);

    connect(m_event_bridge.get(), &MeetingEventBridge::meetingEnded,
            this, &MainWindow::handleMeetingEnded);

    connect(m_event_bridge.get(), &MeetingEventBridge::userVideoEnableChanged,
            this, &MainWindow::handleUserVideoEnableChanged);

    connect(m_event_bridge.get(), &MeetingEventBridge::userAudioEnableChanged,
            this, &MainWindow::handleUserAudioEnableChanged);

    connect(m_event_bridge.get(),
            &MeetingEventBridge::transportConnectionStateChanged,
            this, &MainWindow::handleTransportStateChanged);
}

void MainWindow::handleLoginRequested(const QString& user_name,
                                      const QString& password)
{
    const QString normalized_user_name = user_name.trimmed();

    if (!m_engine_initialized || !m_engine) {
        m_login_page->showMessage(tr("The meeting engine is not initialized."), true);
        m_login_page->setBusy(false);
        return;
    }

    if (normalized_user_name.isEmpty() || password.isEmpty()) {
        m_login_page->showMessage(tr("User name and password cannot be empty."), true);
        m_login_page->setBusy(false);
        return;
    }

    const VCE::Result result =
        m_engine->LoginUser(ToStdString(normalized_user_name), ToStdString(password));

    if (result != VCE::kRet_SUCCESS) {
        showOperationError(tr("Login"), result);
        m_login_page->setBusy(false);
        return;
    }

    m_current_user_name = normalized_user_name;
    m_lobby_page->setCurrentUser(m_current_user_name);
    m_lobby_page->clearMessage();
    m_login_page->setBusy(false);
    showLobbyPage();
}

void MainWindow::handleRegisterRequested(const QString& user_name,
                                         const QString& password)
{
    const QString normalized_user_name = user_name.trimmed();

    if (!m_engine_initialized || !m_engine) {
        m_login_page->showMessage(tr("The meeting engine is not initialized."), true);
        m_login_page->setBusy(false);
        return;
    }

    if (normalized_user_name.isEmpty() || password.isEmpty()) {
        m_login_page->showMessage(tr("User name and password cannot be empty."), true);
        m_login_page->setBusy(false);
        return;
    }

    const VCE::Result result =
        m_engine->RegisterUser(ToStdString(normalized_user_name), ToStdString(password));

    if (result != VCE::kRet_SUCCESS) {
        showOperationError(tr("Register user"), result);
        m_login_page->setBusy(false);
        return;
    }

    m_login_page->showMessage(
        tr("Registration succeeded. You can now log in."),
        false);

    m_login_page->setBusy(false);
}

void MainWindow::handleCreateMeetingRequested(const QString& title,
                                              const QString& description)
{
    if (!m_engine_initialized || !m_engine || m_current_user_name.isEmpty()) {
        m_lobby_page->showMessage(tr("The current user is not logged in."), true);
        m_lobby_page->setBusy(false);
        return;
    }

    if (!applySelectedCaptureDevices()) {
        m_lobby_page->setBusy(false);
        return;
    }

    VCE::CreateMeetingInfo request;
    request.user_name = ToStdString(m_current_user_name);
    request.title = ToStdString(title.trimmed());
    request.description = ToStdString(description.trimmed());
    request.start_time = CurrentUnixTimeSeconds();

    VCE::CreateMeetingResponse response;
    const VCE::Result result = m_engine->CreateMeeting(request, response);

    if (result != VCE::kRet_SUCCESS) {
        showOperationError(tr("Create meeting"), result);
        m_lobby_page->setBusy(false);
        return;
    }

    const QString meeting_id = ToQString(response.meeting_id);

    if (!enterConference(meeting_id, title.trimmed(), true)) {
        VCE::MeetingBriefInfo rollback_request;
        rollback_request.user_name = request.user_name;
        rollback_request.meeting_id = response.meeting_id;
        m_engine->EndMeeting(rollback_request);

        m_lobby_page->showMessage(
            tr("The meeting was created, but the conference page could not be opened."),
            true);

        m_lobby_page->setBusy(false);
        return;
    }

    m_lobby_page->setBusy(false);
}

void MainWindow::handleJoinMeetingRequested(const QString& meeting_id)
{
    const QString normalized_meeting_id = meeting_id.trimmed();

    if (!applySelectedCaptureDevices()) {
        m_lobby_page->setBusy(false);
        return;
    }

    if (!m_engine_initialized || !m_engine || m_current_user_name.isEmpty()) {
        m_lobby_page->showMessage(tr("The current user is not logged in."), true);
        m_lobby_page->setBusy(false);
        return;
    }

    if (normalized_meeting_id.isEmpty()) {
        m_lobby_page->showMessage(tr("Meeting ID cannot be empty."), true);
        m_lobby_page->setBusy(false);
        return;
    }

    VCE::MeetingBriefInfo request;
    request.user_name = ToStdString(m_current_user_name);
    request.meeting_id = ToStdString(normalized_meeting_id);

    VCE::JoinMeetingResponse response;
    const VCE::Result result = m_engine->JoinMeeting(request, response);

    if (result != VCE::kRet_SUCCESS) {
        showOperationError(tr("Join meeting"), result);
        m_lobby_page->setBusy(false);
        return;
    }

    QStringList existing_users;
    existing_users.reserve(static_cast<qsizetype>(response.participants.size()));

    for (const VCE::UserInfo& participant : response.participants) {
        const QString user_name = ToQString(participant.user_name).trimmed();

        if (!user_name.isEmpty() && user_name != m_current_user_name &&
            !existing_users.contains(user_name)) {
            existing_users.append(user_name);
        }
    }

    const QString meeting_title = ToQString(response.meeting_title);

    if (!enterConference(normalized_meeting_id,
                         meeting_title,
                         false,
                         existing_users)) {
        m_engine->LeaveMeeting(request);

        m_lobby_page->showMessage(
            tr("The meeting was joined, but the conference page could not be opened."),
            true);

        m_lobby_page->setBusy(false);
        return;
    }

    m_lobby_page->setBusy(false);
}

void MainWindow::handleCameraToggleRequested(bool enable)
{
    if (!m_meeting_active || !m_engine) {
        m_conference_page->setBusy(false);
        return;
    }

    const VCE::Result result =
        enable ? m_engine->OpenCamera() : m_engine->CloseCamera();

    if (result == VCE::kRet_SUCCESS) {
        m_camera_open = enable;
        m_conference_page->setLocalCameraEnabled(enable);
        m_conference_page->clearMessage();
    } else {
        showOperationError(
            enable ? tr("Open camera") : tr("Close camera"),
            result);
    }

    m_conference_page->setBusy(false);
}

void MainWindow::handleMicrophoneToggleRequested(bool enable)
{
    if (!m_meeting_active || !m_engine) {
        m_conference_page->setBusy(false);
        return;
    }

    const VCE::Result result =
        enable ? m_engine->OpenMic() : m_engine->CloseMic();

    if (result == VCE::kRet_SUCCESS) {
        m_microphone_open = enable;
        m_conference_page->setLocalMicrophoneEnabled(enable);
        m_conference_page->clearMessage();
    } else {
        showOperationError(
            enable ? tr("Open microphone") : tr("Close microphone"),
            result);
    }

    m_conference_page->setBusy(false);
}

void MainWindow::handleSpeakerChangeRequested(const QString& speaker_id)
{
    if (!m_meeting_active || !m_engine) {
        m_conference_page->setBusy(false);
        return;
    }

    const QString normalized_speaker_id = speaker_id.trimmed();

    if (normalized_speaker_id.isEmpty()) {
        m_conference_page->setBusy(false);
        return;
    }

    const VCE::Result result =
        m_engine->UpdateAudioSpeaker(ToStdString(normalized_speaker_id));

    if (result != VCE::kRet_SUCCESS) {
        showOperationError(tr("Change speaker"), result);
    } else {
        m_conference_page->clearMessage();
    }

    /*
     * 无论切换是否成功，都重新查询当前设备，
     * 避免下拉框状态与AudioRender实际设备不一致。
     */
    refreshSpeakerDevices();
    m_conference_page->setBusy(false);
}

void MainWindow::handleLeaveMeetingRequested()
{
    exitCurrentMeeting(false);
}

void MainWindow::handleEndMeetingRequested()
{
    if (!m_is_meeting_creator) {
        m_conference_page->showMessage(
            tr("Only the meeting creator can end this meeting."),
            true);

        m_conference_page->setBusy(false);
        return;
    }

    exitCurrentMeeting(true);
}

void MainWindow::handleUsersJoined(const QStringList& user_names)
{
    if (!m_meeting_active || m_shutting_down) {
        return;
    }

    m_conference_page->addRemoteUsers(user_names);
}

void MainWindow::handleUsersLeft(const QStringList& user_names)
{
    if (!m_meeting_active || m_shutting_down) {
        return;
    }

    m_conference_page->removeRemoteUsers(user_names);
}

void MainWindow::handleMeetingEnded()
{
    if (!m_meeting_active || m_shutting_down) {
        return;
    }

    closeLocalMedia();
    finishMeetingExit(tr("The meeting has ended."));
}

void MainWindow::handleUserVideoEnableChanged(const QString& user_name,
                                              bool enable)
{
    if (!m_meeting_active || m_shutting_down) {
        return;
    }

    m_conference_page->setUserVideoEnabled(user_name, enable);
}

void MainWindow::handleUserAudioEnableChanged(const QString& user_name,
                                              bool enable)
{
    if (!m_meeting_active || m_shutting_down) {
        return;
    }

    m_conference_page->setUserAudioEnabled(user_name, enable);
}

void MainWindow::handleTransportStateChanged(VCE::TransportState state)
{
    if (!m_meeting_active || m_shutting_down) {
        return;
    }

    m_conference_page->setTransportState(state);

    if (state == VCE::TransportState::kFailed) {
        m_conference_page->showMessage(
            tr("The media connection failed."),
            true);
    }
}

void MainWindow::handleRenderInitializationFailed(const QString& user_name,
                                                  int result)
{
    if (!m_meeting_active || m_shutting_down) {
        return;
    }

    m_conference_page->showMessage(
        tr("Failed to initialize the video window for %1 (error %2).")
            .arg(user_name)
            .arg(result),
        true);
}

bool MainWindow::enterConference(const QString& meeting_id,
                                 const QString& meeting_title,
                                 bool is_creator,
                                 const QStringList& existing_users)
{
    if (!m_conference_page->enterMeeting(
            meeting_id,
            meeting_title,
            m_current_user_name,
            is_creator)) {
        return false;
    }

    m_current_meeting_id = meeting_id;
    m_current_meeting_title = meeting_title;
    m_is_meeting_creator = is_creator;
    m_meeting_active = true;
    m_camera_open = false;
    m_microphone_open = false;

    m_conference_page->addRemoteUsers(existing_users);
    m_conference_page->setLocalCameraEnabled(false);
    m_conference_page->setLocalMicrophoneEnabled(false);

    showConferencePage();
    refreshSpeakerDevices();
    m_conference_page->setBusy(false);
    return true;
}

void MainWindow::exitCurrentMeeting(bool end_meeting)
{
    if (!m_meeting_active || !m_engine ||
        m_current_user_name.isEmpty() ||
        m_current_meeting_id.isEmpty()) {
        m_conference_page->setBusy(false);
        return;
    }

    closeLocalMedia();

    VCE::MeetingBriefInfo request;
    request.user_name = ToStdString(m_current_user_name);
    request.meeting_id = ToStdString(m_current_meeting_id);

    const VCE::Result result =
        end_meeting ? m_engine->EndMeeting(request)
                    : m_engine->LeaveMeeting(request);

    if (result != VCE::kRet_SUCCESS) {
        showOperationError(
            end_meeting ? tr("End meeting") : tr("Leave meeting"),
            result);

        m_conference_page->setBusy(false);
        return;
    }

    finishMeetingExit(
        end_meeting ? tr("The meeting was ended.")
                    : tr("You left the meeting."));
}

void MainWindow::closeLocalMedia()
{
    if (!m_engine) {
        m_camera_open = false;
        m_microphone_open = false;
        return;
    }

    if (m_camera_open) {
        m_engine->CloseCamera();
        m_camera_open = false;
    }

    if (m_microphone_open) {
        m_engine->CloseMic();
        m_microphone_open = false;
    }

    if (m_conference_page) {
        m_conference_page->setLocalCameraEnabled(false);
        m_conference_page->setLocalMicrophoneEnabled(false);
    }
}

void MainWindow::finishMeetingExit(const QString& message)
{
    if (m_conference_page) {
        /*
         * 在RenderEngine反初始化前销毁VideoWidget，
         * 使每个窗口有机会在自己的OpenGL上下文中释放渲染资源。
         */
        m_conference_page->clearMeeting();
    }

    m_current_meeting_id.clear();
    m_current_meeting_title.clear();
    m_meeting_active = false;
    m_is_meeting_creator = false;
    m_camera_open = false;
    m_microphone_open = false;

    showLobbyPage();
    m_lobby_page->setBusy(false);

    if (!message.trimmed().isEmpty()) {
        m_lobby_page->showMessage(message, false);
    }
}

void MainWindow::refreshCaptureDevices()
{
    if (!m_engine || !m_engine_initialized) {
        return;
    }

    QStringList errors;

    // ==================== 摄像头 ====================

    std::vector<VCE::CameraDeviceInfo> camera_devices;
    const VCE::Result camera_result =
        m_engine->GetCameraDevices(camera_devices);

    if (camera_result == VCE::kRet_SUCCESS) {
        std::string current_camera_id;

        if (m_engine->GetCurrentCameraDeviceId(
                current_camera_id) !=
            VCE::kRet_SUCCESS) {
            current_camera_id.clear();
        }

        m_lobby_page->setCameraDevices(
            camera_devices,
            ToQString(current_camera_id));

        if (camera_devices.empty()) {
            errors.append(
                tr("No camera devices were found."));
        }
    } else {
        m_lobby_page->setCameraDevices({}, {});

        errors.append(
            tr("Failed to enumerate cameras (error %1).")
                .arg(static_cast<int>(camera_result)));
    }

    // ==================== 麦克风 ====================

    std::vector<VCE::MicDeviceInfo> microphone_devices;
    const VCE::Result microphone_result =
        m_engine->GetMicrophoneDevices(
            microphone_devices);

    if (microphone_result == VCE::kRet_SUCCESS) {
        std::string current_microphone_id;

        if (m_engine->GetCurrentMicrophoneDeviceId(
                current_microphone_id) !=
            VCE::kRet_SUCCESS) {
            current_microphone_id.clear();
        }

        m_lobby_page->setMicrophoneDevices(
            microphone_devices,
            ToQString(current_microphone_id));

        if (microphone_devices.empty()) {
            errors.append(
                tr("No microphone devices were found."));
        }
    } else {
        m_lobby_page->setMicrophoneDevices({}, {});

        errors.append(
            tr("Failed to enumerate microphones (error %1).")
                .arg(static_cast<int>(
                    microphone_result)));
    }

    if (!errors.isEmpty()) {
        m_lobby_page->showMessage(
            errors.join(QLatin1Char('\n')),
            true);
    }
}

bool MainWindow::applySelectedCaptureDevices()
{
    if (!m_engine || !m_engine_initialized) {
        m_lobby_page->showMessage(
            tr("The meeting engine is not initialized."),
            true);

        return false;
    }

    const QString camera_device_id =
        m_lobby_page
            ->selectedCameraDeviceId()
            .trimmed();

    const QString microphone_device_id =
        m_lobby_page
            ->selectedMicrophoneDeviceId()
            .trimmed();

    if (camera_device_id.isEmpty()) {
        m_lobby_page->showMessage(
            tr("Please select a camera device."),
            true);

        return false;
    }

    if (microphone_device_id.isEmpty()) {
        m_lobby_page->showMessage(
            tr("Please select a microphone device."),
            true);

        return false;
    }

    const VCE::Result camera_result =
        m_engine->UpdateCameraDevice(
            ToStdString(camera_device_id));

    if (camera_result != VCE::kRet_SUCCESS) {
        showOperationError(
            tr("Select camera"),
            camera_result);

        return false;
    }

    const VCE::Result microphone_result =
        m_engine->UpdateMicrophoneDevice(
            ToStdString(microphone_device_id));

    if (microphone_result != VCE::kRet_SUCCESS) {
        showOperationError(
            tr("Select microphone"),
            microphone_result);

        return false;
    }

    return true;
}

void MainWindow::refreshSpeakerDevices()
{
    if (!m_engine || !m_meeting_active) {
        return;
    }

    std::vector<VCE::SpeakerDeviceInfo> speakers;
    const VCE::Result devices_result = m_engine->GetAudioSpeakers(speakers);

    if (devices_result != VCE::kRet_SUCCESS) {
        m_conference_page->setSpeakerDevices({}, {});
        showOperationError(tr("Enumerate speakers"), devices_result);
        return;
    }

    std::string current_speaker_id;
    const VCE::Result current_result =
        m_engine->GetCurrentAudioSpeaker(current_speaker_id);

    if (current_result != VCE::kRet_SUCCESS) {
        /*
         * 查询当前设备失败不影响会议进入。
         * ConferencePage会优先选择设备列表中的默认设备。
         */
        current_speaker_id.clear();
    }

    m_conference_page->setSpeakerDevices(
        speakers,
        ToQString(current_speaker_id));
}

void MainWindow::showLoginPage()
{
    if (m_page_stack && m_login_page) {
        m_page_stack->setCurrentWidget(m_login_page);
    }
}

void MainWindow::showLobbyPage()
{
    if (!m_page_stack || !m_lobby_page) {
        return;
    }

    m_lobby_page->setCurrentUser(
        m_current_user_name);

    m_page_stack->setCurrentWidget(
        m_lobby_page);

    /*
     * 登录成功或离开会议返回大厅时刷新设备。
     * 此时摄像头和麦克风均处于关闭状态，
     * 可以安全使用临时OBS source枚举设备。
     */
    if (m_engine_initialized &&
        !m_current_user_name.isEmpty()) {
        refreshCaptureDevices();
    }
}

void MainWindow::showConferencePage()
{
    if (m_page_stack && m_conference_page) {
        m_page_stack->setCurrentWidget(m_conference_page);
    }
}

void MainWindow::showOperationError(const QString& operation,
                                    VCE::Result result)
{
    const QString message =
        tr("%1 failed (error %2).")
            .arg(operation)
            .arg(static_cast<int>(result));

    if (m_page_stack->currentWidget() == m_conference_page) {
        m_conference_page->showMessage(message, true);
    } else if (m_page_stack->currentWidget() == m_lobby_page) {
        m_lobby_page->showMessage(message, true);
    } else {
        m_login_page->showMessage(message, true);
    }
}

void MainWindow::shutdown()
{
    if (m_shutting_down) {
        return;
    }

    m_shutting_down = true;

    /*
     * 先注销观察者，阻止关闭过程中产生新的Qt界面事件。
     */
    if (m_observer_registered && m_engine && m_event_bridge) {
        m_engine->RemoveObserver(m_event_bridge);
        m_observer_registered = false;
    }

    closeLocalMedia();

    if (m_meeting_active && m_engine &&
        !m_current_user_name.isEmpty() &&
        !m_current_meeting_id.isEmpty()) {
        VCE::MeetingBriefInfo request;
        request.user_name = ToStdString(m_current_user_name);
        request.meeting_id = ToStdString(m_current_meeting_id);

        /*
         * 关闭客户端只代表当前用户退出。
         * 即使当前用户是创建者，也不自动结束其他人的会议。
         */
        m_engine->LeaveMeeting(request);
    }

    if (m_conference_page) {
        m_conference_page->clearMeeting();
    }

    m_meeting_active = false;
    m_is_meeting_creator = false;
    m_current_meeting_id.clear();
    m_current_meeting_title.clear();

    if (m_engine_initialized && m_engine) {
        m_engine->UninitSubModel();
        m_engine_initialized = false;
    }

    m_event_bridge.reset();
    m_engine.reset();
    VCE::VceEngine::ReleaseInstance();
}

} // namespace VCE::CLIENT