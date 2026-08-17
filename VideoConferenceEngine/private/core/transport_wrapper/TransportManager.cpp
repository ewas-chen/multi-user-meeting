#include "TransportManager.h"

#include "ITransportEngine.h"
#include "TransportDefine.h"
#include "utils/logManager.h"

#include <exception>
#include <utility>

namespace VCE {

TransportManager::TransportManager() = default;

TransportManager::~TransportManager()
{
    Uninit();
}

Result TransportManager::Initialize(const PublishConfig& config)
{
    if (config.video_width <= 0 || config.video_height <= 0 ||
        config.video_fps <= 0 || config.audio_sample_rate <= 0 ||
        config.audio_channels <= 0) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_initialized.load(std::memory_order_acquire)) {
        return Result::kRet_SUCCESS;
    }

    auto transport_engine = TRANSPORT::ITransportEngine::Create();
    if (!transport_engine) {
        LOG_ERROR("Failed to create TransportEngine");
        return Result::kRet_TransportFailedToConnectServer;
    }

    TRANSPORT::PublishInfo publish_info;
    publish_info.video_width = config.video_width;
    publish_info.video_height = config.video_height;
    publish_info.video_fps = config.video_fps;
    publish_info.audio_sample_rate = config.audio_sample_rate;
    publish_info.audio_channels = config.audio_channels;

    if (!transport_engine->Initialize(publish_info)) {
        LOG_ERROR("Failed to initialize TransportEngine");
        return Result::kRet_TransportFailedToConnectServer;
    }

    transport_engine->RegisterVideoCallback(
        [this](const std::string& user_id,
               const std::shared_ptr<I420Frame>& frame) {
            OnVideoFrameReceived(user_id, frame);
        });

    transport_engine->RegisterAudioCallback(
        [this](const std::string& user_id,
               const std::shared_ptr<AudioFrame>& frame) {
            OnAudioFrameReceived(user_id, frame);
        });

    transport_engine->RegisterConnectionStateCallback(
        [this](TRANSPORT::ConnectionState state) {
            OnConnectionStateChanged(state);
        });

    m_transport_engine = std::move(transport_engine);
    m_initialized.store(true, std::memory_order_release);

    LOG_INFO("TransportManager initialized: video={}x{}@{}fps, audio={}Hz/{}ch",
             config.video_width, config.video_height, config.video_fps,
             config.audio_sample_rate, config.audio_channels);

    return Result::kRet_SUCCESS;
}

void TransportManager::Uninit()
{
    /*
     * 先解除上层回调关系，使正在到达的远端帧不会继续进入
     * VceEngineImpl和RenderManager。
     */
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_transport_callback.reset();
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    m_initialized.store(false, std::memory_order_release);

    if (!m_transport_engine) {
        m_room_info = {};
        return;
    }

    m_transport_engine->RegisterVideoCallback({});
    m_transport_engine->RegisterAudioCallback({});
    m_transport_engine->RegisterConnectionStateCallback({});

    m_transport_engine->Uninit();
    m_transport_engine.reset();
    m_room_info = {};

    LOG_INFO("TransportManager uninitialized");
}

Result TransportManager::SetRoomInfo(const RoomInfo& room_info)
{
    if (room_info.local_user_id.empty() ||
        room_info.room_id.empty() ||
        room_info.media_server.push_server_url.empty() ||
        room_info.media_server.pull_server_url.empty()) {
        LOG_ERROR("Invalid transport room information");
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    TRANSPORT::TransportTargetRoomInfo target_room_info;
    target_room_info.local_user_id = room_info.local_user_id;
    target_room_info.room_id = room_info.room_id;
    target_room_info.push_server_url = room_info.media_server.push_server_url;
        
    target_room_info.pull_server_url = room_info.media_server.pull_server_url;
    
    target_room_info.app_name = room_info.media_server.app_name;

    target_room_info.rtc_external_address = room_info.media_server.rtc_external_address;
        
    target_room_info.whip_secret = room_info.media_server.publish_secret;

    m_transport_engine->SetTargetRoomInfo(target_room_info);
    m_room_info = room_info;

    LOG_INFO("Transport room configured: room={}, local_user={}",
             room_info.room_id, room_info.local_user_id);

    return Result::kRet_SUCCESS;
}

RoomInfo TransportManager::GetRoomInfo() const
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_room_info;
}

Result TransportManager::StartPublishCameraVideo()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    /*
     * 是否已经发布、房间配置是否完整以及编码器是否可用，
     * 统一由TransportEngine检查。
     */
    if (!m_transport_engine->StartPublishVideo()) {
        LOG_ERROR("Failed to start publishing camera video");
        return Result::kRet_TransportFailedToPublish;
    }

    return Result::kRet_SUCCESS;
}

Result TransportManager::StopPublishCameraVideo()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    if (!m_transport_engine->StopPublishVideo()) {
        LOG_ERROR("Failed to stop publishing camera video");
        return Result::kRet_TransportFailedToPublish;
    }

    return Result::kRet_SUCCESS;
}

Result TransportManager::StartPublishAudio()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    if (!m_transport_engine->StartPublishAudio()) {
        LOG_ERROR("Failed to start publishing audio");
        return Result::kRet_TransportFailedToPublish;
    }

    return Result::kRet_SUCCESS;
}

Result TransportManager::StopPublishAudio()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    if (!m_transport_engine->StopPublishAudio()) {
        LOG_ERROR("Failed to stop publishing audio");
        return Result::kRet_TransportFailedToPublish;
    }

    return Result::kRet_SUCCESS;
}

Result TransportManager::PushVideoFrame(
    const std::shared_ptr<I420Frame>& frame,
    CaptureType capture_type)
{
    if (!frame || capture_type == CaptureType::kCT_Unknown) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    /*
     * 不在Manager重复检查I420尺寸、发布状态和编码器状态，
     * TransportEngine::PushVideoFrame()负责完整校验和入队。
     */
    if (!m_transport_engine->PushVideoFrame(frame)) {
        return Result::kRet_TransportFailedToPublish;
    }

    return Result::kRet_SUCCESS;
}

Result TransportManager::PushAudioFrame(
    const std::shared_ptr<AudioFrame>& frame)
{
    if (!frame) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    if (!m_transport_engine->PushAudioFrame(frame)) {
        return Result::kRet_TransportFailedToPublish;
    }

    return Result::kRet_SUCCESS;
}

bool TransportManager::IsPublishingCameraVideo()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    return m_initialized.load(std::memory_order_acquire) &&
           m_transport_engine &&
           m_transport_engine->IsPublishingVideo();
}

bool TransportManager::IsPublishingAudio()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    return m_initialized.load(std::memory_order_acquire) &&
           m_transport_engine &&
           m_transport_engine->IsPublishingAudio();
}

Result TransportManager::SubscribeUserAV(const std::string& user_id)
{
    if (user_id.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    /*
     * 重复订阅、本地用户订阅和房间状态检查由TransportEngine负责。
     */
    if (!m_transport_engine->SubscribeUserAV(user_id)) {
        LOG_ERROR("Failed to subscribe remote user: {}", user_id);
        return Result::kRet_TransportFailedToSubscribe;
    }

    return Result::kRet_SUCCESS;
}

Result TransportManager::UnsubscribeUserAV(const std::string& user_id)
{
    if (user_id.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return Result::kRet_TransportNotInitialized;
    }

    if (!m_transport_engine->UnsubscribeUserAV(user_id)) {
        LOG_ERROR("Failed to unsubscribe remote user: {}", user_id);
        return Result::kRet_TransportFailedToSubscribe;
    }

    return Result::kRet_SUCCESS;
}

Result TransportManager::SubscribeGroupUsers(
    const std::vector<std::string>& user_ids)
{
    Result final_result = Result::kRet_SUCCESS;

    for (const auto& user_id : user_ids) {
        const Result result = SubscribeUserAV(user_id);

        /*
         * 一个用户订阅失败时继续尝试其余用户，
         * 最后向上层返回本次操作存在失败。
         */
        if (result != Result::kRet_SUCCESS) {
            final_result = result;
        }
    }

    return final_result;
}

Result TransportManager::UnsubscribeGroupUsers(
    const std::vector<std::string>& user_ids)
{
    Result final_result = Result::kRet_SUCCESS;

    for (const auto& user_id : user_ids) {
        const Result result = UnsubscribeUserAV(user_id);

        if (result != Result::kRet_SUCCESS) {
            final_result = result;
        }
    }

    return final_result;
}

bool TransportManager::IsUserSubscribedAV(
    const std::string& user_id)
{
    if (user_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    return m_initialized.load(std::memory_order_acquire) &&
           m_transport_engine &&
           m_transport_engine->IsUserSubscribedAV(user_id);
}

std::vector<std::string>
TransportManager::GetSubscribedUsers()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_transport_engine) {
        return {};
    }

    return m_transport_engine->GetSubscribedUsers();
}

Result TransportManager::SetTransportDataCallback(
    const std::shared_ptr<ITransportDataCallback>& callback)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_transport_callback = callback;
    return Result::kRet_SUCCESS;
}

void TransportManager::OnVideoFrameReceived(
    const std::string& user_id,
    const std::shared_ptr<I420Frame>& frame)
{
    if (!m_initialized.load(std::memory_order_acquire) ||
        user_id.empty() || !frame) {
        return;
    }

    std::shared_ptr<ITransportDataCallback> callback;

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback = m_transport_callback.lock();
    }

    if (!callback) {
        return;
    }

    /*
     * 不持有m_callback_mutex调用外部对象，避免回调重新进入
     * Manager时产生死锁。
     */
    try {
        callback->OnTransportVideoFrame(
            user_id,
            CaptureType::kCT_Camera,
            frame);
    } catch (const std::exception& exception) {
        LOG_ERROR("Transport video callback failed: {}",
                  exception.what());
    } catch (...) {
        LOG_ERROR("Transport video callback failed: unknown exception");
    }
}

void TransportManager::OnAudioFrameReceived(
    const std::string& user_id,
    const std::shared_ptr<AudioFrame>& frame)
{
    if (!m_initialized.load(std::memory_order_acquire) ||
        user_id.empty() || !frame) {
        return;
    }

    std::shared_ptr<ITransportDataCallback> callback;

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback = m_transport_callback.lock();
    }

    if (!callback) {
        return;
    }

    try {
        callback->OnTransportAudioFrame(user_id, frame);
    } catch (const std::exception& exception) {
        LOG_ERROR("Transport audio callback failed: {}",
                  exception.what());
    } catch (...) {
        LOG_ERROR("Transport audio callback failed: unknown exception");
    }
}

void TransportManager::OnConnectionStateChanged(
    TRANSPORT::ConnectionState state)
{
    if (!m_initialized.load(std::memory_order_acquire)) {
        return;
    }

    std::shared_ptr<ITransportDataCallback> callback;

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback = m_transport_callback.lock();
    }

    if (!callback) {
        return;
    }

    try {
        callback->OnTransportConnectionStateChanged(
            ConvertConnectionState(state));
    } catch (const std::exception& exception) {
        LOG_ERROR("Transport state callback failed: {}",
                  exception.what());
    } catch (...) {
        LOG_ERROR("Transport state callback failed: unknown exception");
    }
}

TransportState TransportManager::ConvertConnectionState(
    TRANSPORT::ConnectionState state) noexcept
{
    switch (state) {
    case TRANSPORT::ConnectionState::kDisconnected:
        return TransportState::kDisconnected;

    case TRANSPORT::ConnectionState::kConnecting:
        return TransportState::kConnecting;

    case TRANSPORT::ConnectionState::kConnected:
        return TransportState::kConnected;

    case TRANSPORT::ConnectionState::kDisconnecting:
        return TransportState::kDisconnecting;

    case TRANSPORT::ConnectionState::kFailed:
        return TransportState::kFailed;

    case TRANSPORT::ConnectionState::kClosed:
        return TransportState::kClosed;

    default:
        return TransportState::kDisconnected;
    }
}

} // namespace VCE