#include "RtcTransportBase.h"

#include "utils/logManager.h"

#include <exception>
#include <utility>

namespace TRANSPORT {

RtcTransportBase::RtcTransportBase() = default;

RtcTransportBase::~RtcTransportBase() {
    /*
     * 基类析构时，派生类部分已经析构，不能再调用虚函数Close()。
     *这里只清理PeerConnection公共资源。
     */
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_state_change_callback = nullptr;
        m_local_description_callback = nullptr;
    }

    ClosePeerConnection();
}

bool RtcTransportBase::Initialize(const PublishInfo& publish_info) {
    /*
     * 当前H.264和I420实现要求宽高为正数且为偶数。
     * 当前Opus实现支持单声道和双声道。
     */
    if (publish_info.video_width <= 0 ||
        publish_info.video_height <= 0 ||
        publish_info.video_width % 2 != 0 ||
        publish_info.video_height % 2 != 0 ||
        publish_info.video_fps <= 0) {
        LOG_ERROR(
            "Invalid video publish information: {}x{} @ {} fps",
            publish_info.video_width,
            publish_info.video_height,
            publish_info.video_fps);
        return false;
    }

    if (publish_info.audio_sample_rate <= 0 ||
        publish_info.audio_channels < 1 ||
        publish_info.audio_channels > 2) {
        LOG_ERROR(
            "Invalid audio publish information: sample_rate={}, channels={}",
            publish_info.audio_sample_rate,
            publish_info.audio_channels);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_peer_connection) {
        LOG_ERROR("Cannot initialize RTC transport while PeerConnection is active");
        return false;
    }

    m_publish_info = publish_info;
    m_initialized = true;

    return true;
}

bool RtcTransportBase::Open() {
    std::shared_ptr<rtc::PeerConnection> connection;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_initialized) {
            LOG_ERROR("RtcTransportBase is not initialized");
            return false;
        }

        /*
         * Open只负责创建PeerConnection。
         * 此时连接还没有完成，因此不能用IsOpen()判断是否已经创建。
         */
        if (m_peer_connection) {
            return true;
        }

        try {
            rtc::Configuration configuration;

            /*
             * 正式部署时建议将STUN/TURN地址放入外部配置，
             * 不要长期硬编码在传输模块中。
             */
            configuration.iceServers.emplace_back(
                "stun:stun.l.google.com:19302");

            connection =
                std::make_shared<rtc::PeerConnection>(configuration);

            if (!connection) {
                LOG_ERROR("Failed to create RTC PeerConnection");
                return false;
            }

            m_peer_connection = connection;
        } catch (const std::exception& exception) {
            LOG_ERROR(
                "Failed to create RTC PeerConnection: {}",
                exception.what());
            m_peer_connection.reset();
            return false;
        } catch (...) {
            LOG_ERROR("Failed to create RTC PeerConnection: unknown exception");
            m_peer_connection.reset();
            return false;
        }
    }

    /*
     * 回调由libdatachannel内部线程执行。
     * 回调中只处理状态和轻量信息，不应执行耗时的编解码或HTTP请求。
     */
    connection->onLocalCandidate(
        [](rtc::Candidate candidate) {
            LOG_DEBUG(
                "Local ICE candidate collected: mid={}, candidate={}",
                candidate.mid(),
                static_cast<std::string>(candidate));
        });

    connection->onIceStateChange(
        [](rtc::PeerConnection::IceState state) {
            LOG_DEBUG(
                "ICE connection state changed: {}",
                static_cast<int>(state));
        });

    connection->onStateChange(
        [this](rtc::PeerConnection::State state) {
            HandleConnectionState(state);
        });

    connection->onGatheringStateChange(
        [this](rtc::PeerConnection::GatheringState state) {
            HandleGatheringState(state);
        });

    SetState(ConnectionState::kDisconnected);

    LOG_INFO(
        "RTC PeerConnection created: video={}x{}@{}fps, audio={}Hz/{}ch",
        m_publish_info.video_width,
        m_publish_info.video_height,
        m_publish_info.video_fps,
        m_publish_info.audio_sample_rate,
        m_publish_info.audio_channels);

    return true;
}

bool RtcTransportBase::SetRemoteDescription(
    const std::string& sdp,
    const std::string& type) {
    if (sdp.empty()) {
        LOG_ERROR("Remote SDP is empty");
        return false;
    }

    if (type.empty()) {
        LOG_ERROR("Remote SDP type is empty");
        return false;
    }

    std::shared_ptr<rtc::PeerConnection> connection;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        connection = m_peer_connection;
    }

    if (!connection) {
        LOG_ERROR("Cannot set remote SDP: PeerConnection is null");
        return false;
    }

    try {
        rtc::Description description(sdp, type);
        connection->setRemoteDescription(std::move(description));

        LOG_INFO("Remote SDP was set successfully: type={}", type);
        return true;
    } catch (const std::exception& exception) {
        LOG_ERROR(
            "Failed to set remote SDP: type={}, error={}",
            type,
            exception.what());
        SetState(ConnectionState::kFailed);
        return false;
    } catch (...) {
        LOG_ERROR(
            "Failed to set remote SDP: type={}, unknown exception",
            type);
        SetState(ConnectionState::kFailed);
        return false;
    }
}

bool RtcTransportBase::IsOpen() const noexcept {
    return m_state.load(std::memory_order_acquire) ==
           ConnectionState::kConnected;
}

ConnectionState RtcTransportBase::GetState() const noexcept {
    return m_state.load(std::memory_order_acquire);
}

void RtcTransportBase::SetStateChangeCallback(
    OnRtcStateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_state_change_callback = std::move(callback);
}

void RtcTransportBase::SetLocalDescriptionCallback(
    OnLocalDescriptionCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_local_description_callback = std::move(callback);
}

PublishInfo RtcTransportBase::GetPublishInfo() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_publish_info;
}

void RtcTransportBase::SetUserId(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_user_id = user_id;
}

std::string RtcTransportBase::GetUserId() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_user_id;
}

void RtcTransportBase::SetState(ConnectionState state) {
    const ConnectionState previous =
        m_state.exchange(state, std::memory_order_acq_rel);

    // 相同状态不重复通知上层。
    if (previous == state) {
        return;
    }

    OnRtcStateChangeCallback callback;

    try {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback = m_state_change_callback;
    } catch (...) {
        return;
    }

    if (!callback) {
        return;
    }

    /*
     * 外部回调不能让异常进入libdatachannel内部线程。
     */
    try {
        callback(state);
    } catch (const std::exception& exception) {
        LOG_ERROR(
            "RTC state callback threw an exception: {}",
            exception.what());
    } catch (...) {
        LOG_ERROR("RTC state callback threw an unknown exception");
    }
}

void RtcTransportBase::HandleConnectionState(
    rtc::PeerConnection::State state) {
    LOG_DEBUG(
        "PeerConnection state changed: {}",
        static_cast<int>(state));

    switch (state) {
    case rtc::PeerConnection::State::New:
        SetState(ConnectionState::kDisconnected);
        break;

    case rtc::PeerConnection::State::Connecting:
        SetState(ConnectionState::kConnecting);
        break;

    case rtc::PeerConnection::State::Connected:
        LOG_INFO("RTC PeerConnection connected");
        SetState(ConnectionState::kConnected);
        break;

    case rtc::PeerConnection::State::Disconnected:
        LOG_WARN("RTC PeerConnection disconnected");
        SetState(ConnectionState::kDisconnected);
        break;

    case rtc::PeerConnection::State::Failed:
        LOG_ERROR("RTC PeerConnection failed");
        SetState(ConnectionState::kFailed);
        break;

    case rtc::PeerConnection::State::Closed:
        LOG_INFO("RTC PeerConnection closed");
        SetState(ConnectionState::kClosed);
        break;
    }
}

void RtcTransportBase::HandleGatheringState(
    rtc::PeerConnection::GatheringState state) {
    LOG_DEBUG(
        "ICE gathering state changed: {}",
        static_cast<int>(state));

    if (state !=
        rtc::PeerConnection::GatheringState::Complete) {
        return;
    }

    std::shared_ptr<rtc::PeerConnection> connection;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        connection = m_peer_connection;
    }

    if (!connection) {
        return;
    }

    try {
        const auto local_description =
            connection->localDescription();

        if (!local_description) {
            LOG_ERROR(
                "ICE gathering completed but local SDP is unavailable");
            SetState(ConnectionState::kFailed);
            return;
        }

        OnLocalDescriptionCallback callback;

        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            callback = m_local_description_callback;
        }

        if (!callback) {
            LOG_WARN(
                "Local SDP is ready but no callback is registered");
            return;
        }

        LOG_INFO(
            "ICE gathering completed, local SDP is ready: type={}",
            rtc::Description::typeToString(
                local_description->type()));

        /*
         * 当前回调运行在libdatachannel内部线程。
         * 后续TransportEngine收到SDP后，应转交给信令工作线程，
         * 不要直接在这里执行可能长时间阻塞的HTTP请求。
         */
        callback(*local_description);
    } catch (const std::exception& exception) {
        LOG_ERROR(
            "Failed to process local SDP: {}",
            exception.what());
        SetState(ConnectionState::kFailed);
    } catch (...) {
        LOG_ERROR(
            "Failed to process local SDP: unknown exception");
        SetState(ConnectionState::kFailed);
    }
}

void RtcTransportBase::ClosePeerConnection() noexcept {
    std::shared_ptr<rtc::PeerConnection> connection;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        connection = std::move(m_peer_connection);
    }

    if (!connection) {
        return;
    }

    SetState(ConnectionState::kDisconnecting);

    try {
        /*
         * 先移除所有libdatachannel回调，防止关闭过程中继续访问当前对象。
         */
        connection->resetCallbacks();
        connection->close();
    } catch (const std::exception& exception) {
        LOG_ERROR(
            "Failed to close RTC PeerConnection: {}",
            exception.what());
    } catch (...) {
        LOG_ERROR(
            "Failed to close RTC PeerConnection: unknown exception");
    }

    SetState(ConnectionState::kClosed);
}

} // namespace TRANSPORT