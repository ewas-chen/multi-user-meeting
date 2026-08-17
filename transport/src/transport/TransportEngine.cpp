#include "TransportEngine.h"

#include "RtcPullTransport.h"
#include "RtcPushTransport.h"
#include "utils/logManager.h"

#include <rtc/description.hpp>
#include <rtc/rtc.h>

#include <exception>
#include <utility>

namespace TRANSPORT {

std::unique_ptr<ITransportEngine> ITransportEngine::Create() {
    return std::make_unique<TransportEngine>();
}

TransportEngine::TransportEngine() = default;

TransportEngine::~TransportEngine() {
    Uninit();
}

bool TransportEngine::Initialize(const PublishInfo& publish_info) {
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    rtcPreload();

    auto push_transport = std::make_shared<RtcPushTransport>();

    if (!push_transport->Initialize(publish_info)) {
        LOG_ERROR("Failed to initialize RtcPushTransport");
        rtcCleanup();
        return false;
    }

    try {
        m_signaling_client =
            std::make_unique<WhipWhepClient>();
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to create signaling client: {}",
                  exception.what());
        rtcCleanup();
        return false;
    }

    /*
     * 本地SDP回调运行在libdatachannel内部线程。
     * HandlePublishLocalDescription只生成SDP并投递信令任务。
     */
    push_transport->SetLocalDescriptionCallback(
        [this](const rtc::Description& description) {
            HandlePublishLocalDescription(description);
        });

    push_transport->SetStateChangeCallback(
        [this](ConnectionState state) {
            m_connection_state.store(
                state,
                std::memory_order_release);

            ConnectionStateCallback callback;

            {
                std::lock_guard<std::mutex> callback_lock(
                    m_callback_mutex);
                callback = m_connection_state_callback;
            }

            if (!callback) {
                return;
            }

            try {
                callback(state);
            } catch (const std::exception& exception) {
                LOG_ERROR("Connection state callback failed: {}",
                          exception.what());
            } catch (...) {
                LOG_ERROR(
                    "Connection state callback failed: unknown exception");
            }
        });

    m_push_transport = std::move(push_transport);
    m_publish_info = publish_info;
    m_publish_resource_url.clear();
    m_publish_generation = 0;

    if (!StartSignalingWorker()) {
        LOG_ERROR("Failed to start signaling worker");
        m_push_transport.reset();
        m_signaling_client.reset();
        rtcCleanup();
        return false;
    }

    m_initialized.store(true, std::memory_order_release);

    LOG_INFO("TransportEngine initialized");
    return true;
}

void TransportEngine::Uninit() {
    if (!m_initialized.exchange(
            false,
            std::memory_order_acq_rel)) {
        return;
    }

    /*
     * 先停止信令线程，等待正在执行的HTTP请求结束。
     * m_initialized已经为false，异步任务不会再把Answer
     * 应用到PeerConnection。
     */
    StopSignalingWorker();

    ClosePublish();
    CloseAllPull();

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_video_callback = nullptr;
        m_audio_callback = nullptr;
        m_connection_state_callback = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_push_transport.reset();
        m_signaling_client.reset();
        m_publish_resource_url.clear();
        m_publish_config = {};
        m_room_info = {};
    }

    m_connection_state.store(
        ConnectionState::kDisconnected,
        std::memory_order_release);

    rtcCleanup();

    LOG_INFO("TransportEngine uninitialized");
}

void TransportEngine::SetTargetRoomInfo(
    const TransportTargetRoomInfo& config) {
    std::shared_ptr<RtcPushTransport> push_transport;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_room_info = config;
        push_transport = m_push_transport;
    }

    if (push_transport) {
        push_transport->SetUserId(config.local_user_id);
    }
}

TransportTargetRoomInfo
TransportEngine::GetTargetRoomInfo() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_room_info;
}

bool TransportEngine::StartPublishVideo() {
    std::shared_ptr<RtcPushTransport> push_transport;
    std::string local_user_id;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_initialized.load(std::memory_order_acquire) ||
            !m_push_transport) {
            return false;
        }

        if (m_room_info.push_server_url.empty() ||
            m_room_info.room_id.empty() ||
            m_room_info.local_user_id.empty()) {
            LOG_ERROR("Cannot publish video: room configuration is incomplete");
            return false;
        }

        push_transport = m_push_transport;
        local_user_id = m_room_info.local_user_id;

        /*
         * 只有音视频都未发布时，才认为这是一个新的WHIP会话。
         * 后续启动另一种媒体时继续复用同一个PeerConnection。
         */
        if (!push_transport->IsPublishingVideo() &&
            !push_transport->IsPublishingAudio()) {
            ++m_publish_generation;

            m_publish_config = {};
            m_publish_config.endpoint_url = m_room_info.push_server_url;
            m_publish_config.app_name = "live";
            m_publish_config.room_id = m_room_info.room_id;
            m_publish_config.local_user_id = m_room_info.local_user_id;
                
            m_publish_config.rtc_external_address = m_room_info.rtc_external_address;

            m_publish_config.secret = m_room_info.whip_secret;
            /*
            * 当前Oryx明确要求Bearer Token为空。
            */
            m_publish_config.authorization_token.clear();

            m_publish_resource_url.clear();
        }
    }

    push_transport->SetUserId(local_user_id);
    return push_transport->StartPublishVideo(local_user_id);
}

bool TransportEngine::StopPublishVideo() {
    std::shared_ptr<RtcPushTransport> push_transport;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        push_transport = m_push_transport;
    }

    if (!push_transport) {
        return true;
    }

    const bool result =
        push_transport->StopPublishVideo();

    /*
     * 音频仍在发送时保留PeerConnection和WHIP资源。
     */
    if (!push_transport->IsPublishingAudio()) {
        ClosePublish();
    }

    return result;
}

bool TransportEngine::StartPublishAudio() {
    std::shared_ptr<RtcPushTransport> push_transport;
    std::string local_user_id;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_initialized.load(std::memory_order_acquire) ||
            !m_push_transport) {
            return false;
        }

        if (m_room_info.push_server_url.empty() ||
            m_room_info.room_id.empty() ||
            m_room_info.local_user_id.empty()) {
            LOG_ERROR("Cannot publish audio: room configuration is incomplete");
            return false;
        }

        push_transport = m_push_transport;
        local_user_id = m_room_info.local_user_id;

        if (!push_transport->IsPublishingVideo() &&
            !push_transport->IsPublishingAudio()) {
            ++m_publish_generation;

            m_publish_config = {};
            m_publish_config.endpoint_url =
                m_room_info.push_server_url;
            m_publish_config.room_id =
                m_room_info.room_id;
            m_publish_config.local_user_id =
                m_room_info.local_user_id;

            m_publish_resource_url.clear();
        }
    }

    push_transport->SetUserId(local_user_id);
    return push_transport->StartPublishAudio(local_user_id);
}

bool TransportEngine::StopPublishAudio() {
    std::shared_ptr<RtcPushTransport> push_transport;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        push_transport = m_push_transport;
    }

    if (!push_transport) {
        return true;
    }

    const bool result =
        push_transport->StopPublishAudio();

    if (!push_transport->IsPublishingVideo()) {
        ClosePublish();
    }

    return result;
}

bool TransportEngine::PushVideoFrame(
    const std::shared_ptr<I420Frame>& frame) {
    if (!m_initialized.load(std::memory_order_acquire) ||
        !frame ||
        !frame->IsValid()) {
        return false;
    }

    std::shared_ptr<RtcPushTransport> push_transport;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        push_transport = m_push_transport;
    }

    return push_transport &&
           push_transport->PushVideoFrame(frame);
}

bool TransportEngine::PushAudioFrame(
    const std::shared_ptr<AudioFrame>& frame) {
    if (!m_initialized.load(std::memory_order_acquire) ||
        !frame ||
        !frame->IsValid()) {
        return false;
    }

    std::shared_ptr<RtcPushTransport> push_transport;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        push_transport = m_push_transport;
    }

    return push_transport &&
           push_transport->PushAudioFrame(frame);
}

bool TransportEngine::IsPublishingVideo() const {
    std::shared_ptr<RtcPushTransport> push_transport;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        push_transport = m_push_transport;
    }

    return push_transport &&
           push_transport->IsPublishingVideo();
}

bool TransportEngine::IsPublishingAudio() const {
    std::shared_ptr<RtcPushTransport> push_transport;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        push_transport = m_push_transport;
    }

    return push_transport &&
           push_transport->IsPublishingAudio();
}

bool TransportEngine::SubscribeUserAV(
    const std::string& user_id) {
    if (!m_initialized.load(std::memory_order_acquire) ||
        user_id.empty()) {
        return false;
    }

    TransportTargetRoomInfo room_info;
    PublishInfo publish_info;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        room_info = m_room_info;
        publish_info = m_publish_info;
    }

    if (room_info.pull_server_url.empty() ||
        room_info.room_id.empty()) {
        LOG_ERROR("Cannot subscribe user: room configuration is incomplete");
        return false;
    }
    
    // 禁止订阅本地用户
    // if (user_id == room_info.local_user_id) {
    //     LOG_ERROR("Cannot subscribe local user: {}", user_id);
    //     return false;
    // }

    {
        std::lock_guard<std::mutex> lock(m_pull_mutex);

        if (m_pull_sessions.find(user_id) !=
            m_pull_sessions.end()) {
            return true;
        }
    }

    auto pull_transport =
        std::make_shared<RtcPullTransport>();

    if (!pull_transport->Initialize(publish_info)) {
        LOG_ERROR("Failed to initialize pull transport: user={}",
                  user_id);
        return false;
    }

    pull_transport->SetUserId(user_id);

    pull_transport->SetLocalDescriptionCallback(
        [this, user_id](
            const rtc::Description& description) {
            HandlePullLocalDescription(
                user_id,
                description);
        });

    pull_transport->SetVideoFrameCallback(
        [this, user_id](
            const std::shared_ptr<I420Frame>& frame) {
            if (!m_initialized.load(
                    std::memory_order_acquire)) {
                return;
            }

            VideoDataCallback callback;

            {
                std::lock_guard<std::mutex> lock(
                    m_callback_mutex);
                callback = m_video_callback;
            }

            if (!callback) {
                return;
            }

            try {
                callback(user_id, frame);
            } catch (const std::exception& exception) {
                LOG_ERROR("Video data callback failed: {}",
                          exception.what());
            } catch (...) {
                LOG_ERROR(
                    "Video data callback failed: unknown exception");
            }
        });

    pull_transport->SetAudioFrameCallback(
        [this, user_id](
            const std::shared_ptr<AudioFrame>& frame) {
            if (!m_initialized.load(
                    std::memory_order_acquire)) {
                return;
            }

            AudioDataCallback callback;

            {
                std::lock_guard<std::mutex> lock(
                    m_callback_mutex);
                callback = m_audio_callback;
            }

            if (!callback) {
                return;
            }

            try {
                callback(user_id, frame);
            } catch (const std::exception& exception) {
                LOG_ERROR("Audio data callback failed: {}",
                          exception.what());
            } catch (...) {
                LOG_ERROR(
                    "Audio data callback failed: unknown exception");
            }
        });

    SubscribeConfig config;
    config.endpoint_url = room_info.pull_server_url;
    config.room_id = room_info.room_id;
    config.remote_user_id = user_id;
    config.app_name = "live";

    config.rtc_external_address = room_info.rtc_external_address;

    config.authorization_token.clear();

    {
        std::lock_guard<std::mutex> lock(m_pull_mutex);

        /*
         * 再次检查，防止两个线程同时订阅同一用户。
         */
        if (m_pull_sessions.find(user_id) !=
            m_pull_sessions.end()) {
            return true;
        }

        PullSession session;
        session.transport = pull_transport;
        session.config = config;

        m_pull_sessions.emplace(
            user_id,
            std::move(session));
    }

    /*
     * 必须先加入m_pull_sessions，再创建Offer。
     * 本地SDP回调后需要通过user_id找到对应会话。
     */
    if (!pull_transport->SubscribeAudioVideo()) {
        {
            std::lock_guard<std::mutex> lock(m_pull_mutex);
            m_pull_sessions.erase(user_id);
        }

        pull_transport->Close();

        LOG_ERROR("Failed to start user subscription: {}",
                  user_id);
        return false;
    }

    LOG_INFO("Remote user subscription created: {}",
             user_id);

    return true;
}

bool TransportEngine::UnsubscribeUserAV(
    const std::string& user_id) {
    PullSession session;

    {
        std::lock_guard<std::mutex> lock(m_pull_mutex);

        auto iterator =
            m_pull_sessions.find(user_id);

        if (iterator == m_pull_sessions.end()) {
            return false;
        }

        session = std::move(iterator->second);
        m_pull_sessions.erase(iterator);
    }

    /*
     * 先关闭本地Track，立即停止数据回调。
     * 然后再执行可能阻塞的HTTP DELETE。
     */
    if (session.transport) {
        session.transport->Close();
    }

    WhipWhepClient* signaling_client = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        signaling_client = m_signaling_client.get();
    }

    bool result = true;

    if (!session.resource_url.empty() &&
        signaling_client) {
        result = signaling_client->Unsubscribe(
            session.resource_url,
            session.config);
    }

    LOG_INFO("Remote user unsubscribed: {}",
             user_id);

    return result;
}

bool TransportEngine::IsUserSubscribedAV(
    const std::string& user_id) const {
    std::lock_guard<std::mutex> lock(m_pull_mutex);

    return m_pull_sessions.find(user_id) !=
           m_pull_sessions.end();
}

std::vector<std::string>
TransportEngine::GetSubscribedUsers() const {
    std::vector<std::string> users;

    std::lock_guard<std::mutex> lock(m_pull_mutex);
    users.reserve(m_pull_sessions.size());

    for (const auto& [user_id, session] :
         m_pull_sessions) {
        users.push_back(user_id);
    }

    return users;
}

void TransportEngine::RegisterVideoCallback(
    VideoDataCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_video_callback = std::move(callback);
}

void TransportEngine::RegisterAudioCallback(
    AudioDataCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_audio_callback = std::move(callback);
}

void TransportEngine::RegisterConnectionStateCallback(
    ConnectionStateCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_connection_state_callback = std::move(callback);
}

bool TransportEngine::IsInitialized() const {
    return m_initialized.load(std::memory_order_acquire);
}

void TransportEngine::HandlePublishLocalDescription(
    const rtc::Description& description) {
    if (!m_initialized.load(std::memory_order_acquire) ||
        description.type() !=
            rtc::Description::Type::Offer) {
        return;
    }

    std::string offer_sdp;

    try {
        offer_sdp = description.generateSdp();
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to generate WHIP Offer SDP: {}",
                  exception.what());
        return;
    }

    if (offer_sdp.empty()) {
        LOG_ERROR("Generated WHIP Offer SDP is empty");
        return;
    }

    PublishConfig config;
    std::uint64_t generation = 0;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        config = m_publish_config;
        generation = m_publish_generation;
    }

    const bool posted = PostSignalingTask(
        [this,
         config = std::move(config),
         offer_sdp = std::move(offer_sdp),
         generation]() {
            WhipWhepClient* client = nullptr;

            {
                std::lock_guard<std::mutex> lock(
                    m_state_mutex);

                if (!m_initialized.load(
                        std::memory_order_acquire) ||
                    generation !=
                        m_publish_generation) {
                    return;
                }

                client = m_signaling_client.get();
            }

            if (!client) {
                return;
            }

            const auto response =
                client->Publish(config, offer_sdp);

            if (!response) {
                LOG_ERROR("WHIP publish request failed");
                ClosePublish();
                return;
            }

            std::shared_ptr<RtcPushTransport> push_transport;
            bool valid_session = false;

            {
                std::lock_guard<std::mutex> lock(
                    m_state_mutex);

                valid_session =
                    m_initialized.load(
                        std::memory_order_acquire) &&
                    generation ==
                        m_publish_generation;

                push_transport = m_push_transport;
            }

            /*
             * StopPublish可能在HTTP请求过程中被调用。
             * 此时不能把旧Answer设置到已关闭或重启的连接。
             */
            if (!valid_session ||
                !push_transport ||
                (!push_transport->IsPublishingVideo() &&
                 !push_transport->IsPublishingAudio())) {
                if (!response->resource_url.empty()) {
                    client->Unpublish(
                        response->resource_url,
                        config);
                }

                return;
            }

            if (!push_transport->SetRemoteDescription(
                    response->answer_sdp,
                    "answer")) {
                if (!response->resource_url.empty()) {
                    client->Unpublish(
                        response->resource_url,
                        config);
                }

                ClosePublish();
                return;
            }

            bool stored = false;

            {
                std::lock_guard<std::mutex> lock(
                    m_state_mutex);

                if (generation ==
                    m_publish_generation) {
                    m_publish_resource_url =
                        response->resource_url;
                    stored = true;
                }
            }

            if (!stored &&
                !response->resource_url.empty()) {
                client->Unpublish(
                    response->resource_url,
                    config);
                return;
            }

            LOG_INFO("WHIP publish connection established");
        });

    if (!posted) {
        LOG_ERROR("Failed to queue WHIP signaling task");
    }
}

void TransportEngine::HandlePullLocalDescription(
    const std::string& user_id,
    const rtc::Description& description) {
    if (!m_initialized.load(std::memory_order_acquire) ||
        description.type() !=
            rtc::Description::Type::Offer) {
        return;
    }

    std::string offer_sdp;

    try {
        offer_sdp = description.generateSdp();
    } catch (const std::exception& exception) {
        LOG_ERROR("Failed to generate WHEP Offer SDP: {}",
                  exception.what());
        return;
    }

    if (offer_sdp.empty()) {
        return;
    }

    SubscribeConfig config;
    std::weak_ptr<RtcPullTransport> weak_transport;

    {
        std::lock_guard<std::mutex> lock(m_pull_mutex);

        auto iterator =
            m_pull_sessions.find(user_id);

        if (iterator == m_pull_sessions.end()) {
            return;
        }

        config = iterator->second.config;
        weak_transport = iterator->second.transport;
    }

    const bool posted = PostSignalingTask(
        [this,
         user_id,
         config = std::move(config),
         offer_sdp = std::move(offer_sdp),
         weak_transport]() {
            auto pull_transport =
                weak_transport.lock();

            if (!pull_transport) {
                return;
            }

            /*
             * HTTP请求前再次确认用户没有取消订阅。
             */
            {
                std::lock_guard<std::mutex> lock(
                    m_pull_mutex);

                auto iterator =
                    m_pull_sessions.find(user_id);

                if (iterator ==
                        m_pull_sessions.end() ||
                    iterator->second.transport !=
                        pull_transport) {
                    return;
                }
            }

            WhipWhepClient* client = nullptr;

            {
                std::lock_guard<std::mutex> lock(
                    m_state_mutex);
                client = m_signaling_client.get();
            }

            if (!client) {
                return;
            }

            const auto response =
                client->Subscribe(config, offer_sdp);

            if (!response) {
                LOG_ERROR("WHEP subscribe request failed: user={}",
                          user_id);

                {
                    std::lock_guard<std::mutex> lock(
                        m_pull_mutex);

                    auto iterator =
                        m_pull_sessions.find(user_id);

                    if (iterator !=
                            m_pull_sessions.end() &&
                        iterator->second.transport ==
                            pull_transport) {
                        m_pull_sessions.erase(iterator);
                    }
                }

                pull_transport->Close();
                return;
            }

            bool active_session = false;

            {
                std::lock_guard<std::mutex> lock(
                    m_pull_mutex);

                auto iterator =
                    m_pull_sessions.find(user_id);

                active_session =
                    iterator != m_pull_sessions.end() &&
                    iterator->second.transport ==
                        pull_transport;
            }

            if (!active_session) {
                if (!response->resource_url.empty()) {
                    client->Unsubscribe(
                        response->resource_url,
                        config);
                }

                return;
            }

            if (!pull_transport->SetRemoteDescription(
                    response->answer_sdp,
                    "answer")) {
                if (!response->resource_url.empty()) {
                    client->Unsubscribe(
                        response->resource_url,
                        config);
                }

                {
                    std::lock_guard<std::mutex> lock(
                        m_pull_mutex);

                    auto iterator =
                        m_pull_sessions.find(user_id);

                    if (iterator !=
                            m_pull_sessions.end() &&
                        iterator->second.transport ==
                            pull_transport) {
                        m_pull_sessions.erase(iterator);
                    }
                }

                pull_transport->Close();
                return;
            }

            bool stored = false;

            {
                std::lock_guard<std::mutex> lock(
                    m_pull_mutex);

                auto iterator =
                    m_pull_sessions.find(user_id);

                if (iterator !=
                        m_pull_sessions.end() &&
                    iterator->second.transport ==
                        pull_transport) {
                    iterator->second.resource_url =
                        response->resource_url;
                    stored = true;
                }
            }

            if (!stored &&
                !response->resource_url.empty()) {
                client->Unsubscribe(
                    response->resource_url,
                    config);
                return;
            }

            LOG_INFO("WHEP subscription established: user={}",
                     user_id);
        });

    if (!posted) {
        LOG_ERROR("Failed to queue WHEP signaling task: user={}",
                  user_id);
    }
}

void TransportEngine::ClosePublish() {
    std::shared_ptr<RtcPushTransport> push_transport;
    WhipWhepClient* signaling_client = nullptr;
    std::string resource_url;
    PublishConfig config;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        ++m_publish_generation;

        push_transport = m_push_transport;
        signaling_client = m_signaling_client.get();
        resource_url = std::move(m_publish_resource_url);
        config = m_publish_config;

        m_publish_resource_url.clear();
    }

    if (push_transport) {
        push_transport->Close();
    }

    if (!resource_url.empty() &&
        signaling_client) {
        signaling_client->Unpublish(
            resource_url,
            config);
    }

    m_connection_state.store(
        ConnectionState::kDisconnected,
        std::memory_order_release);
}

void TransportEngine::CloseAllPull() {
    std::unordered_map<std::string, PullSession> sessions;

    {
        std::lock_guard<std::mutex> lock(m_pull_mutex);
        sessions.swap(m_pull_sessions);
    }

    WhipWhepClient* signaling_client = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        signaling_client = m_signaling_client.get();
    }

    for (auto& [user_id, session] : sessions) {
        if (session.transport) {
            session.transport->Close();
        }

        if (!session.resource_url.empty() &&
            signaling_client) {
            signaling_client->Unsubscribe(
                session.resource_url,
                session.config);
        }
    }
}

bool TransportEngine::StartSignalingWorker() {
    std::lock_guard<std::mutex> lock(m_signaling_mutex);

    if (m_signaling_running) {
        return true;
    }

    m_signaling_running = true;

    try {
        m_signaling_worker =
            std::thread(
                &TransportEngine::SignalingWorkerLoop,
                this);
    } catch (const std::exception& exception) {
        m_signaling_running = false;

        LOG_ERROR("Failed to create signaling thread: {}",
                  exception.what());
        return false;
    }

    return true;
}

void TransportEngine::StopSignalingWorker() {
    {
        std::lock_guard<std::mutex> lock(m_signaling_mutex);

        if (!m_signaling_running &&
            !m_signaling_worker.joinable()) {
            return;
        }

        m_signaling_running = false;

        /*
         * Uninit期间不再执行尚未开始的WHIP/WHEP请求。
         * 当前正在执行的请求会受libcurl超时限制并正常结束。
         */
        m_signaling_tasks.clear();
    }

    m_signaling_cv.notify_all();

    if (m_signaling_worker.joinable()) {
        m_signaling_worker.join();
    }
}

bool TransportEngine::PostSignalingTask(
    std::function<void()> task) {
    if (!task) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_signaling_mutex);

        if (!m_signaling_running) {
            return false;
        }

        m_signaling_tasks.push_back(
            std::move(task));
    }

    m_signaling_cv.notify_one();
    return true;
}

void TransportEngine::SignalingWorkerLoop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(
                m_signaling_mutex);

            m_signaling_cv.wait(
                lock,
                [this]() {
                    return !m_signaling_running ||
                           !m_signaling_tasks.empty();
                });

            if (!m_signaling_running) {
                return;
            }

            task = std::move(
                m_signaling_tasks.front());

            m_signaling_tasks.pop_front();
        }

        try {
            task();
        } catch (const std::exception& exception) {
            LOG_ERROR("Signaling task failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR(
                "Signaling task failed: unknown exception");
        }
    }
}

} // namespace TRANSPORT