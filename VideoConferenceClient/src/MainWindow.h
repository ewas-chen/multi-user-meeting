#pragma once

#include "VceEngine.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QMainWindow>

#include <memory>

class QCloseEvent;
class QStackedWidget;

namespace VCE::CLIENT {

class ConferencePage;
class LoginPage;
class MeetingEventBridge;
class MeetingLobbyPage;

/**
 * @brief Qt客户端主窗口
 *
 * MainWindow负责：
 * - 初始化和释放VceEngine；
 * - 管理登录页、会议大厅页和会议页；
 * - 执行页面发出的用户操作；
 * - 将MeetingEventBridge事件转发到会议页面。
 *
 * 页面只负责显示状态和发送操作意图，
 * 不直接调用会议业务、采集或传输接口。
 */
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(VCE::EngineConfig engine_config, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    /**
     * @brief 初始化会议引擎并注册事件观察者
     *
     * 应在main.cpp创建MainWindow后、调用show()前执行。
     */
    bool initialize();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // 创建页面和主窗口布局
    void setupUi();

    // 连接页面操作信号
    void connectPageSignals();

    // 连接MeetingEventBridge转发的会议事件
    void connectMeetingEvents();

    // ==================== 页面操作处理 ====================

    void handleLoginRequested(const QString& user_name, const QString& password);
    void handleRegisterRequested(const QString& user_name, const QString& password);

    void handleCreateMeetingRequested(const QString& title,
                                      const QString& description);

    void handleJoinMeetingRequested(const QString& meeting_id);

    void handleCameraToggleRequested(bool enable);
    void handleMicrophoneToggleRequested(bool enable);
    void handleSpeakerChangeRequested(const QString& speaker_id);

    void handleLeaveMeetingRequested();
    void handleEndMeetingRequested();

    // ==================== 引擎事件处理 ====================

    void handleUsersJoined(const QStringList& user_names);
    void handleUsersLeft(const QStringList& user_names);
    void handleMeetingEnded();

    void handleUserVideoEnableChanged(const QString& user_name, bool enable);
    void handleUserAudioEnableChanged(const QString& user_name, bool enable);

    void handleTransportStateChanged(VCE::TransportState state);
    void handleRenderInitializationFailed(const QString& user_name, int result);

    // ==================== 会议状态管理 ====================

    /**
     * @brief 进入会议页面
     *
     * existing_users来自JoinMeetingResponse中的已有参会者。
     */
    bool enterConference(const QString& meeting_id,
                         const QString& meeting_title,
                         bool is_creator,
                         const QStringList& existing_users = {});

    /**
     * @brief 执行离开或结束会议
     *
     * end_meeting为true时调用EndMeeting，否则调用LeaveMeeting。
     */
    void exitCurrentMeeting(bool end_meeting);

    // 关闭当前已经开启的本地摄像头和麦克风
    void closeLocalMedia();

    // 清理客户端会议状态并返回会议大厅
    void finishMeetingExit(const QString& message = {});

    // 枚举摄像头和麦克风并更新会议大厅
    void refreshCaptureDevices();

    // 在创建或加入会议前应用大厅中选择的设备
    bool applySelectedCaptureDevices();

    // 查询扬声器设备及当前设备并更新会议页
    void refreshSpeakerDevices();

    // ==================== 页面切换及错误处理 ====================

    void showLoginPage();
    void showLobbyPage();
    void showConferencePage();

    void showOperationError(const QString& operation, VCE::Result result);

    // 注销观察者、退出会议并释放引擎
    void shutdown();

private:
    VCE::EngineConfig m_engine_config;

    std::shared_ptr<VCE::VceEngine> m_engine;
    std::shared_ptr<MeetingEventBridge> m_event_bridge;

    QStackedWidget* m_page_stack{nullptr};
    LoginPage* m_login_page{nullptr};
    MeetingLobbyPage* m_lobby_page{nullptr};
    ConferencePage* m_conference_page{nullptr};

    QString m_current_user_name;
    QString m_current_meeting_id;
    QString m_current_meeting_title;

    bool m_engine_initialized{false};
    bool m_observer_registered{false};
    bool m_meeting_active{false};
    bool m_is_meeting_creator{false};

    bool m_camera_open{false};
    bool m_microphone_open{false};

    // 防止析构、关闭窗口和会议事件重复执行资源清理
    bool m_shutting_down{false};
};

} // namespace VCE::CLIENT