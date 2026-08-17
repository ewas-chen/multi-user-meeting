#include "MeetingEventBridge.h"

#include <QtCore/QMetaObject>

#include <utility>

namespace {

QString ToQString(const std::string& value) {
    return QString::fromUtf8(
        value.data(),
        static_cast<qsizetype>(value.size()));
}

QStringList ToQStringList(
    const std::vector<std::string>& values) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));

    for (const std::string& value : values) {
        result.emplace_back(ToQString(value));
    }

    return result;
}

} // namespace

namespace VCE::CLIENT {

MeetingEventBridge::MeetingEventBridge()
    : QObject(nullptr) {
    /*
     * TransportState作为Qt信号参数使用。
     * 注册后可以安全通过Qt队列连接进行跨线程传递。
     */
    qRegisterMetaType<VCE::TransportState>(
        "VCE::TransportState");
}

void MeetingEventBridge::OnUserJoined(
    const std::vector<std::string>& user_names) {
    if (user_names.empty()) {
        return;
    }

    QStringList names = ToQStringList(user_names);

    /*
     * 回调可能来自gRPC线程。
     * invokeMethod使用当前QObject作为上下文，将实际信号发送
     * 投递到MeetingEventBridge所属的Qt线程。
     */
    QMetaObject::invokeMethod(
        this,
        [this, names = std::move(names)]() {
            emit usersJoined(names);
        },
        Qt::QueuedConnection);
}

void MeetingEventBridge::OnUserLeft(
    const std::vector<std::string>& user_names) {
    if (user_names.empty()) {
        return;
    }

    QStringList names = ToQStringList(user_names);

    QMetaObject::invokeMethod(
        this,
        [this, names = std::move(names)]() {
            emit usersLeft(names);
        },
        Qt::QueuedConnection);
}

void MeetingEventBridge::OnMeetingEnded() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
            emit meetingEnded();
        },
        Qt::QueuedConnection);
}

void MeetingEventBridge::OnUserVideoEnable(
    const std::string& user_name,
    bool enable) {
    if (user_name.empty()) {
        return;
    }

    QString name = ToQString(user_name);

    QMetaObject::invokeMethod(
        this,
        [this, name = std::move(name), enable]() {
            emit userVideoEnableChanged(name, enable);
        },
        Qt::QueuedConnection);
}

void MeetingEventBridge::OnUserAudioEnable(
    const std::string& user_name,
    bool enable) {
    if (user_name.empty()) {
        return;
    }

    QString name = ToQString(user_name);

    QMetaObject::invokeMethod(
        this,
        [this, name = std::move(name), enable]() {
            emit userAudioEnableChanged(name, enable);
        },
        Qt::QueuedConnection);
}

void MeetingEventBridge::OnTransportConnectionStateChanged(
    VCE::TransportState state) {
    QMetaObject::invokeMethod(
        this,
        [this, state]() {
            emit transportConnectionStateChanged(state);
        },
        Qt::QueuedConnection);
}

} // namespace VCE::CLIENT