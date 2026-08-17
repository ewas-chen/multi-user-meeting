#pragma once

#include "IMeetingEventObserver.h"

#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

Q_DECLARE_METATYPE(VCE::TransportState)

namespace VCE::CLIENT {

/**
 * @brief 将会议引擎后台事件转换为Qt界面信号
 *
 * IMeetingEventObserver回调可能来自gRPC、RTC或媒体线程，
 * MeetingEventBridge不会直接操作界面。具体实现通过Qt事件队列
 * 将通知投递到该对象所属的UI线程，再发出对应信号。
 *
 * 该对象需要由std::shared_ptr管理，以便注册到VceEngine。
 * 不要同时设置QObject父对象，避免Qt父子对象机制与shared_ptr
 * 重复释放同一个对象。
 */
class MeetingEventBridge final
    : public QObject,
      public VCE::IMeetingEventObserver {
    Q_OBJECT

public:
    MeetingEventBridge();
    ~MeetingEventBridge() override = default;

    MeetingEventBridge(const MeetingEventBridge&) = delete;
    MeetingEventBridge& operator=(const MeetingEventBridge&) = delete;
    MeetingEventBridge(MeetingEventBridge&&) = delete;
    MeetingEventBridge& operator=(MeetingEventBridge&&) = delete;

    // ==================== IMeetingEventObserver ====================

    void OnUserJoined(
        const std::vector<std::string>& user_names) override;

    void OnUserLeft(
        const std::vector<std::string>& user_names) override;

    void OnMeetingEnded() override;

    void OnUserVideoEnable(
        const std::string& user_name,
        bool enable) override;

    void OnUserAudioEnable(
        const std::string& user_name,
        bool enable) override;

    void OnTransportConnectionStateChanged(
        VCE::TransportState state) override;

signals:
    // 以下信号统一在MeetingEventBridge所属的Qt线程中发出。
    void usersJoined(const QStringList& user_names);
    void usersLeft(const QStringList& user_names);
    void meetingEnded();

    void userVideoEnableChanged(
        const QString& user_name,
        bool enable);

    void userAudioEnableChanged(
        const QString& user_name,
        bool enable);

    void transportConnectionStateChanged(
        VCE::TransportState state);
};

} // namespace VCE::CLIENT