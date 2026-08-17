#include "VceEngineImpl.h"

#include "CaptureManager.h"
#include "RenderManager.h"
#include "ServiceManager.h"
#include "TransportManager.h"
#include "utils/logManager.h"

#include <algorithm>
#include <exception>
#include <unordered_set>
#include <utility>

namespace VCE {

VceEngineImpl::VceEngineImpl() = default;

VceEngineImpl::~VceEngineImpl() {
    UninitSubModel();
}

// ==================== 引擎生命周期 ====================

Result VceEngineImpl::InitSubModel(const EngineConfig& config) {
    if (config.sample_rate <= 0 || config.channels <= 0 ||
        config.video_width <= 0 || config.video_height <= 0 ||
        config.video_fps <= 0 || config.service.server_address.empty()) {
        LOG_ERROR("VceEngine initialization failed: invalid configuration");
        return kRet_InvalidParam;
    }

    const auto self = weak_from_this().lock();
    if (!self) {
        LOG_ERROR("VceEngineImpl must be managed by shared_ptr");
        return kRet_Invalid_Status;
    }

    std::unique_lock<std::mutex> state_lock(m_state_mutex);

    if (m_submodel_initialized.load(std::memory_order_acquire)) {
        return kRet_SUCCESS;
    }

    auto service_manager = std::make_shared<SERVICE::ServiceManager>();
    auto render_manager = std::make_shared<RenderManager>();
    auto transport_manager = std::make_shared<TransportManager>();
    auto capture_manager = std::make_shared<CaptureManager>();

    Result result = service_manager->Initialize(config.service);
    if (result != kRet_SUCCESS) {
        return result;
    }

    result = render_manager->Initialize(config.sample_rate, config.channels,
                                        config.video_width, config.video_height);
    if (result != kRet_SUCCESS) {
        service_manager->Uninitialize();
        return result;
    }

    PublishConfig publish_config;
    publish_config.video_width = config.video_width;
    publish_config.video_height = config.video_height;
    publish_config.video_fps = config.video_fps;
    publish_config.audio_sample_rate = config.sample_rate;
    publish_config.audio_channels = config.channels;

    result = transport_manager->Initialize(publish_config);
    if (result != kRet_SUCCESS) {
        render_manager->Uninit();
        service_manager->Uninitialize();
        return result;
    }

    result = capture_manager->Initialize(config.sample_rate, config.channels,
                                         config.video_width, config.video_height,
                                         config.video_fps);
    if (result != kRet_SUCCESS) {
        transport_manager->Uninit();
        render_manager->Uninit();
        service_manager->Uninitialize();
        return result;
    }

    result = capture_manager->SetCaptureDataCallback(self);
    if (result != kRet_SUCCESS) {
        capture_manager->Uninit();
        transport_manager->Uninit();
        render_manager->Uninit();
        service_manager->Uninitialize();
        return result;
    }

    result = transport_manager->SetTransportDataCallback(self);
    if (result != kRet_SUCCESS) {
        capture_manager->SetCaptureDataCallback(nullptr);
        capture_manager->Uninit();
        transport_manager->Uninit();
        render_manager->Uninit();
        service_manager->Uninitialize();
        return result;
    }

    m_service_manager = service_manager;
    m_capture_manager = std::move(capture_manager);
    m_render_manager = std::move(render_manager);
    m_transport_manager = std::move(transport_manager);

    m_current_meeting_id.clear();
    m_local_user_name.clear();
    m_camera_open = false;
    m_microphone_open = false;

    /*
     * 初始化完成不代表已经进入会议。
     * 创建或加入会议并成功设置RTC房间后才接受媒体帧。
     */
    m_accept_media_callbacks.store(false, std::memory_order_release);
    m_submodel_initialized.store(true, std::memory_order_release);

    /*
     * 不在持有m_state_mutex时注册服务事件回调。
     * 服务实现可能在注册过程中启动事件线程。
     */
    state_lock.unlock();
    service_manager->SetServiceDataCallback(self);

    LOG_INFO("VceEngineImpl initialized");
    return kRet_SUCCESS;
}

Result VceEngineImpl::UninitSubModel() {
    if (!m_submodel_initialized.exchange(false, std::memory_order_acq_rel)) {
        return kRet_SUCCESS;
    }

    m_accept_media_callbacks.store(false, std::memory_order_release);

    std::shared_ptr<SERVICE::ServiceManager> service_snapshot;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        service_snapshot = m_service_manager;
    }

    /*
     * 先停止业务事件进入，避免反初始化期间继续新增订阅。
     */
    if (service_snapshot) {
        service_snapshot->SetServiceDataCallback(nullptr);
    }

    /*
     * Qt调用本函数前必须先销毁所有VideoRenderWidget，
     * 因而这里不直接操作任何OpenGL用户资源。
     */
    CleanupMeetingMedia();

    std::shared_ptr<SERVICE::ServiceManager> service_manager;
    std::shared_ptr<CaptureManager> capture_manager;
    std::shared_ptr<RenderManager> render_manager;
    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        service_manager = std::move(m_service_manager);
        capture_manager = std::move(m_capture_manager);
        render_manager = std::move(m_render_manager);
        transport_manager = std::move(m_transport_manager);

        m_current_meeting_id.clear();
        m_local_user_name.clear();
        m_camera_open = false;
        m_microphone_open = false;
    }

    if (capture_manager) {
        capture_manager->SetCaptureDataCallback(nullptr);
        capture_manager->Uninit();
    }

    if (transport_manager) {
        transport_manager->SetTransportDataCallback(nullptr);
        transport_manager->Uninit();
    }

    /*
     * VideoRenderWidget应当已经删除所有用户，因此RenderManager
     * 此时只需停止AudioRender、AudioMixer并释放空的渲染引擎。
     */
    if (render_manager) {
        render_manager->Uninit();
    }

    if (service_manager) {
        service_manager->Uninitialize();
    }

    {
        std::lock_guard<std::mutex> lock(m_media_state_mutex);
        m_user_media_states.clear();
    }

    LOG_INFO("VceEngineImpl uninitialized");
    return kRet_SUCCESS;
}

// ==================== 用户服务 ====================

Result VceEngineImpl::RegisterUser(const std::string& user_name,
                                   const std::string& password) {
    std::shared_ptr<SERVICE::ServiceManager> service_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        service_manager = m_service_manager;
    }

    if (!m_submodel_initialized.load(std::memory_order_acquire) ||
        !service_manager) {
        return kRet_Invalid_Status;
    }

    return service_manager->RegisterUser(user_name, password);
}

Result VceEngineImpl::LoginUser(const std::string& user_name,
                                const std::string& password) {
    std::shared_ptr<SERVICE::ServiceManager> service_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        service_manager = m_service_manager;
    }

    if (!m_submodel_initialized.load(std::memory_order_acquire) ||
        !service_manager) {
        return kRet_Invalid_Status;
    }

    return service_manager->LoginUser(user_name, password);
}

// ==================== 会议管理 ====================

Result VceEngineImpl::CreateMeeting(const CreateMeetingInfo& request,
                                    CreateMeetingResponse& response) {
    std::shared_ptr<SERVICE::ServiceManager> service_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_submodel_initialized.load(std::memory_order_acquire) ||
            !m_current_meeting_id.empty()) {
            return kRet_Invalid_Status;
        }

        service_manager = m_service_manager;
    }

    if (!service_manager) {
        return kRet_Invalid_Status;
    }

    Result result = service_manager->CreateMeeting(request, response);
    if (result != kRet_SUCCESS) {
        return result;
    }

    result = SetRoomInfo(response.meeting_id, request.user_name,
                         response.media_server);
    if (result != kRet_SUCCESS) {
        MeetingBriefInfo rollback_request;
        rollback_request.user_name = request.user_name;
        rollback_request.meeting_id = response.meeting_id;

        service_manager->EndMeeting(rollback_request);
        return result;
    }

    /*
     * 不能在会议业务调用中创建OpenGL资源。
     * Qt进入会议页后，由本地VideoRenderWidget::initializeGL()
     * 调用AddUser(request.user_name, true)。
     */
    LOG_INFO("Local user created meeting: meeting_id={}, user={}",
             response.meeting_id, request.user_name);

    return kRet_SUCCESS;
}

Result VceEngineImpl::JoinMeeting(const MeetingBriefInfo& request,
                                  JoinMeetingResponse& response) {
    std::shared_ptr<SERVICE::ServiceManager> service_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_submodel_initialized.load(std::memory_order_acquire) ||
            !m_current_meeting_id.empty()) {
            return kRet_Invalid_Status;
        }

        service_manager = m_service_manager;
    }

    if (!service_manager) {
        return kRet_Invalid_Status;
    }

    Result result = service_manager->JoinMeeting(request, response);
    if (result != kRet_SUCCESS) {
        return result;
    }

    result = SetRoomInfo(request.meeting_id, request.user_name,
                         response.media_server);
    if (result != kRet_SUCCESS) {
        service_manager->LeaveMeeting(request);
        return result;
    }

    /*
     * 这里只订阅JoinMeeting响应中已经存在的远端用户。
     * OpenGL用户由Qt收到NotifyUsersJoined后创建。
     */
    std::unordered_set<std::string> unique_users;
    std::vector<std::string> subscribed_users;

    for (const UserInfo& participant : response.participants) {
        const std::string& user_name = participant.user_name;

        if (user_name.empty() || user_name == request.user_name ||
            !unique_users.emplace(user_name).second) {
            continue;
        }

        result = SubscribeRemoteUserMedia(user_name);
        if (result != kRet_SUCCESS) {
            for (const std::string& subscribed_user : subscribed_users) {
                UnsubscribeRemoteUserMedia(subscribed_user);
            }

            CleanupMeetingMedia();
            service_manager->LeaveMeeting(request);
            return result;
        }

        subscribed_users.emplace_back(user_name);
    }

    if (!subscribed_users.empty()) {
        NotifyUsersJoined(subscribed_users);
    }

    LOG_INFO("Local user joined meeting: meeting_id={}, user={}, remote_users={}",
             request.meeting_id, request.user_name, subscribed_users.size());

    return kRet_SUCCESS;
}

Result VceEngineImpl::LeaveMeeting(const MeetingBriefInfo& request) {
    std::shared_ptr<SERVICE::ServiceManager> service_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_submodel_initialized.load(std::memory_order_acquire) ||
            request.meeting_id != m_current_meeting_id ||
            request.user_name != m_local_user_name) {
            return kRet_InvalidParam;
        }

        service_manager = m_service_manager;
    }

    if (!service_manager) {
        return kRet_Invalid_Status;
    }

    const Result result = service_manager->LeaveMeeting(request);
    if (result != kRet_SUCCESS) {
        return result;
    }

    /*
     * Qt在调用LeaveMeeting前已经销毁视频控件。
     * 这里只停止采集、发布和订阅。
     */
    CleanupMeetingMedia();

    LOG_INFO("Local user left meeting: meeting_id={}, user={}",
             request.meeting_id, request.user_name);

    return kRet_SUCCESS;
}

Result VceEngineImpl::EndMeeting(const MeetingBriefInfo& request) {
    std::shared_ptr<SERVICE::ServiceManager> service_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_submodel_initialized.load(std::memory_order_acquire) ||
            request.meeting_id != m_current_meeting_id ||
            request.user_name != m_local_user_name) {
            return kRet_InvalidParam;
        }

        service_manager = m_service_manager;
    }

    if (!service_manager) {
        return kRet_Invalid_Status;
    }

    const Result result = service_manager->EndMeeting(request);
    if (result != kRet_SUCCESS) {
        return result;
    }

    CleanupMeetingMedia();

    /*
     * 本地创建者已经知道结束请求成功，Qt会主动退出会议页。
     * 不在这里再次NotifyMeetingEnded，避免与服务端结束事件重复。
     */
    LOG_INFO("Meeting ended: meeting_id={}, user={}",
             request.meeting_id, request.user_name);

    return kRet_SUCCESS;
}

Result VceEngineImpl::GetMeetingList(
    const GetMeetingListRequest& request,
    GetMeetingListResponse& response) {
    std::shared_ptr<SERVICE::ServiceManager> service_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        service_manager = m_service_manager;
    }

    if (!m_submodel_initialized.load(std::memory_order_acquire) ||
        !service_manager) {
        return kRet_Invalid_Status;
    }

    return service_manager->GetMeetingList(request, response);
}

// ==================== 采集设备管理 ====================

Result VceEngineImpl::GetCameraDevices(
    std::vector<CameraDeviceInfo>& devices) {
    std::shared_ptr<CaptureManager> capture_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        capture_manager = m_capture_manager;
    }

    if (!capture_manager) {
        return kRet_CaptureNotInitialized;
    }

    return capture_manager->GetCameraDevices(devices);
}

Result VceEngineImpl::GetMicrophoneDevices(
    std::vector<MicDeviceInfo>& devices) {
    std::shared_ptr<CaptureManager> capture_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        capture_manager = m_capture_manager;
    }

    if (!capture_manager) {
        return kRet_CaptureNotInitialized;
    }

    return capture_manager->GetMicrophoneDevices(devices);
}

Result VceEngineImpl::GetCurrentCameraDeviceId(
    std::string& camera_device_id) {
    std::shared_ptr<CaptureManager> capture_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        capture_manager = m_capture_manager;
    }

    if (!capture_manager) {
        return kRet_CaptureNotInitialized;
    }

    return capture_manager->GetCurrentCameraDeviceId(camera_device_id);
}

Result VceEngineImpl::GetCurrentMicrophoneDeviceId(
    std::string& microphone_device_id) {
    std::shared_ptr<CaptureManager> capture_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        capture_manager = m_capture_manager;
    }

    if (!capture_manager) {
        return kRet_CaptureNotInitialized;
    }

    return capture_manager->GetCurrentMicrophoneDeviceId(
        microphone_device_id);
}

Result VceEngineImpl::UpdateCameraDevice(
    const std::string& camera_device_id) {
    std::shared_ptr<CaptureManager> capture_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        capture_manager = m_capture_manager;
    }

    if (!capture_manager) {
        return kRet_CaptureNotInitialized;
    }

    return capture_manager->UpdateCameraDevice(camera_device_id);
}

Result VceEngineImpl::UpdateMicrophoneDevice(
    const std::string& microphone_device_id) {
    std::shared_ptr<CaptureManager> capture_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        capture_manager = m_capture_manager;
    }

    if (!capture_manager) {
        return kRet_CaptureNotInitialized;
    }

    return capture_manager->UpdateMicrophoneDevice(
        microphone_device_id);
}

Result VceEngineImpl::OpenCamera() {
    std::shared_ptr<CaptureManager> capture_manager;
    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_submodel_initialized.load(std::memory_order_acquire) ||
            m_current_meeting_id.empty()) {
            return kRet_Invalid_Status;
        }

        if (m_camera_open) {
            return kRet_SUCCESS;
        }

        capture_manager = m_capture_manager;
        transport_manager = m_transport_manager;
    }

    if (!capture_manager || !transport_manager) {
        return kRet_CaptureNotInitialized;
    }

    Result result = transport_manager->StartPublishCameraVideo();
    if (result != kRet_SUCCESS) {
        return result;
    }

    result = capture_manager->OpenCamera();
    if (result != kRet_SUCCESS) {
        transport_manager->StopPublishCameraVideo();
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_camera_open = true;
    }

    return kRet_SUCCESS;
}

Result VceEngineImpl::CloseCamera() {
    std::shared_ptr<CaptureManager> capture_manager;
    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_camera_open) {
            return kRet_SUCCESS;
        }

        capture_manager = m_capture_manager;
        transport_manager = m_transport_manager;
    }

    Result capture_result = kRet_SUCCESS;
    Result transport_result = kRet_SUCCESS;

    if (capture_manager) {
        capture_result = capture_manager->CloseCamera();
    }

    if (transport_manager) {
        transport_result = transport_manager->StopPublishCameraVideo();
    }

    if (capture_result == kRet_SUCCESS) {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_camera_open = false;
    }

    if (capture_result != kRet_SUCCESS) {
        return capture_result;
    }

    return transport_result;
}

Result VceEngineImpl::OpenMic() {
    std::shared_ptr<CaptureManager> capture_manager;
    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_submodel_initialized.load(std::memory_order_acquire) ||
            m_current_meeting_id.empty()) {
            return kRet_Invalid_Status;
        }

        if (m_microphone_open) {
            return kRet_SUCCESS;
        }

        capture_manager = m_capture_manager;
        transport_manager = m_transport_manager;
    }

    if (!capture_manager || !transport_manager) {
        return kRet_CaptureNotInitialized;
    }

    Result result = transport_manager->StartPublishAudio();
    if (result != kRet_SUCCESS) {
        return result;
    }

    result = capture_manager->OpenMic();
    if (result != kRet_SUCCESS) {
        transport_manager->StopPublishAudio();
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_microphone_open = true;
    }

    return kRet_SUCCESS;
}

Result VceEngineImpl::CloseMic() {
    std::shared_ptr<CaptureManager> capture_manager;
    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_microphone_open) {
            return kRet_SUCCESS;
        }

        capture_manager = m_capture_manager;
        transport_manager = m_transport_manager;
    }

    Result capture_result = kRet_SUCCESS;
    Result transport_result = kRet_SUCCESS;

    if (capture_manager) {
        capture_result = capture_manager->CloseMic();
    }

    if (transport_manager) {
        transport_result = transport_manager->StopPublishAudio();
    }

    if (capture_result == kRet_SUCCESS) {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_microphone_open = false;
    }

    if (capture_result != kRet_SUCCESS) {
        return capture_result;
    }

    return transport_result;
}

// ==================== 渲染管理 ====================

Result VceEngineImpl::AddUser(const std::string& user_name,
                              bool is_local) {
    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return kRet_RenderNotInitialized;
    }

    /*
     * 本函数由QOpenGLWidget::initializeGL()调用。
     */
    return render_manager->AddUser(user_name, is_local);
}

Result VceEngineImpl::RemoveUser(const std::string& user_name) {
    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return kRet_RenderNotInitialized;
    }

    /*
     * 只删除渲染资源以及RenderEngine中该用户已有的音频缓冲。
     * RTC退订和会议用户通知由会议媒体管理流程负责。
     */
    return render_manager->RemoveUser(user_name);
}

Result VceEngineImpl::RenderUser(const std::string& user_name) {
    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return kRet_RenderNotInitialized;
    }

    return render_manager->RenderUserFrame(user_name);
}

Result VceEngineImpl::UpdateUserVideoSize(
    const std::string& user_name,
    int width,
    int height) {
    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return kRet_RenderNotInitialized;
    }

    return render_manager->UpdateUserVideoSize(user_name, width, height);
}

// ==================== 音频输出设备 ====================

Result VceEngineImpl::GetAudioSpeakers(
    std::vector<SpeakerDeviceInfo>& speakers) {
    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return kRet_RenderNotInitialized;
    }

    return render_manager->GetAudioSpeakers(speakers);
}

Result VceEngineImpl::UpdateAudioSpeaker(
    const std::string& speaker_id) {
    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return kRet_RenderNotInitialized;
    }

    return render_manager->UpdateAudioSpeaker(speaker_id);
}

Result VceEngineImpl::GetCurrentAudioSpeaker(
    std::string& speaker_id) {
    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return kRet_RenderNotInitialized;
    }

    return render_manager->GetCurrentAudioSpeaker(speaker_id);
}

// ==================== 采集模块回调 ====================

void VceEngineImpl::OnCaptureVideoFrame(
    const std::shared_ptr<I420Frame>& frame) {
    if (!m_accept_media_callbacks.load(std::memory_order_acquire) ||
        !frame) {
        return;
    }

    std::shared_ptr<RenderManager> render_manager;
    std::shared_ptr<TransportManager> transport_manager;
    std::string local_user_name;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
        transport_manager = m_transport_manager;
        local_user_name = m_local_user_name;
    }

    if (render_manager && !local_user_name.empty()) {
        render_manager->PushVideoFrame(local_user_name, frame);
    }

    if (transport_manager) {
        transport_manager->PushVideoFrame(
            frame, CaptureType::kCT_Camera);
    }
}

void VceEngineImpl::OnCaptureAudioFrame(
    const std::shared_ptr<AudioFrame>& frame) {
    if (!m_accept_media_callbacks.load(std::memory_order_acquire) ||
        !frame) {
        return;
    }

    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        transport_manager = m_transport_manager;
    }

    /*
     * 保持原有逻辑：本地麦克风不送入RenderManager，
     * 避免扬声器回放产生回声。
     */
    if (transport_manager) {
        transport_manager->PushAudioFrame(frame);
    }
}

// ==================== 传输模块回调 ====================

void VceEngineImpl::OnTransportVideoFrame(
    const std::string& user_id,
    CaptureType capture_type,
    const std::shared_ptr<I420Frame>& frame) {
    if (!m_accept_media_callbacks.load(std::memory_order_acquire) ||
        user_id.empty() || !frame) {
        return;
    }

    (void)capture_type;

    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return;
    }

    if (render_manager->PushVideoFrame(user_id, frame) == kRet_SUCCESS) {
        MarkUserVideoReceived(user_id);
    }
}

void VceEngineImpl::OnTransportAudioFrame(
    const std::string& user_id,
    const std::shared_ptr<AudioFrame>& frame) {
    if (!m_accept_media_callbacks.load(std::memory_order_acquire) ||
        user_id.empty() || !frame) {
        return;
    }

    std::shared_ptr<RenderManager> render_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        render_manager = m_render_manager;
    }

    if (!render_manager) {
        return;
    }

    /*
     * 保持现有音频链路不变：
     * TransportManager -> RenderManager -> AudioMixer。
     */
    if (render_manager->PushAudioFrame(user_id, frame) == kRet_SUCCESS) {
        MarkUserAudioReceived(user_id);
    }
}

void VceEngineImpl::OnTransportConnectionStateChanged(
    TransportState state) {
    NotifyTransportStateChanged(state);
}

// ==================== 业务服务事件回调 ====================

void VceEngineImpl::OnUserJoined(const std::string& user_name) {
    if (user_name.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_submodel_initialized.load(std::memory_order_acquire) ||
            m_current_meeting_id.empty() ||
            user_name == m_local_user_name) {
            return;
        }
    }

    const Result result = SubscribeRemoteUserMedia(user_name);
    if (result != kRet_SUCCESS) {
        LOG_ERROR("Failed to subscribe joined user: user={}, result={}",
                  user_name, static_cast<int>(result));
        return;
    }

    /*
     * Qt观察者收到通知后，在UI线程创建VideoRenderWidget。
     */
    NotifyUsersJoined({user_name});
}

void VceEngineImpl::OnUserLeft(const std::string& user_name) {
    if (user_name.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (m_current_meeting_id.empty() ||
            user_name == m_local_user_name) {
            return;
        }
    }

    UnsubscribeRemoteUserMedia(user_name);

    /*
     * Qt观察者收到通知后，在对应OpenGL Context中删除渲染用户。
     */
    NotifyUsersLeft({user_name});
}

void VceEngineImpl::OnMeetingEnded() {
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (m_current_meeting_id.empty()) {
            return;
        }
    }

    CleanupMeetingMedia();
    NotifyMeetingEnded();
}

// ==================== 会议媒体管理 ====================

Result VceEngineImpl::SetRoomInfo(
    const std::string& meeting_id,
    const std::string& local_user_name,
    const MediaServerInfo& media_server) {
    if (meeting_id.empty() || local_user_name.empty() ||
        media_server.push_server_url.empty() ||
        media_server.pull_server_url.empty() ||
        media_server.app_name.empty() ||
        media_server.rtc_external_address.empty()) {
        return kRet_InvalidParam;
    }

    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        if (!m_current_meeting_id.empty()) {
            return kRet_Invalid_Status;
        }

        transport_manager = m_transport_manager;
    }

    if (!transport_manager) {
        return kRet_TransportNotInitialized;
    }

    RoomInfo room_info;
    room_info.local_user_id = local_user_name;
    room_info.room_id = meeting_id;
    room_info.media_server = media_server;

    const Result result = transport_manager->SetRoomInfo(room_info);
    if (result != kRet_SUCCESS) {
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_current_meeting_id = meeting_id;
        m_local_user_name = local_user_name;
    }

    m_accept_media_callbacks.store(true, std::memory_order_release);
    return kRet_SUCCESS;
}

Result VceEngineImpl::SubscribeRemoteUserMedia(
    const std::string& user_name) {
    if (user_name.empty()) {
        return kRet_InvalidParam;
    }

    std::shared_ptr<TransportManager> transport_manager;
    std::string local_user_name;
    bool meeting_active = false;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        transport_manager = m_transport_manager;
        local_user_name = m_local_user_name;
        meeting_active = !m_current_meeting_id.empty();
    }

    if (!meeting_active || !transport_manager) {
        return kRet_Invalid_Status;
    }

    if (user_name == local_user_name) {
        return kRet_SUCCESS;
    }

    if (!transport_manager->IsUserSubscribedAV(user_name)) {
        const Result result =
            transport_manager->SubscribeUserAV(user_name);

        if (result != kRet_SUCCESS) {
            return result;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_media_state_mutex);
        m_user_media_states.try_emplace(user_name);
    }

    return kRet_SUCCESS;
}

void VceEngineImpl::UnsubscribeRemoteUserMedia(
    const std::string& user_name) {
    if (user_name.empty()) {
        return;
    }

    std::shared_ptr<TransportManager> transport_manager;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        transport_manager = m_transport_manager;
    }

    if (transport_manager &&
        transport_manager->IsUserSubscribedAV(user_name)) {
        const Result result =
            transport_manager->UnsubscribeUserAV(user_name);

        if (result != kRet_SUCCESS) {
            LOG_WARN("Failed to unsubscribe remote user: user={}, result={}",
                     user_name, static_cast<int>(result));
        }
    }

    /*
     * 这里只清理会议层媒体状态。
     * AudioMixer用户缓冲由Qt删除VideoRenderWidget时，
     * 通过VceEngine::RemoveUser()复用现有逻辑清理。
     */
    RemoveUserMediaState(user_name);
}

void VceEngineImpl::CleanupMeetingMedia() {
    m_accept_media_callbacks.store(false, std::memory_order_release);

    std::shared_ptr<CaptureManager> capture_manager;
    std::shared_ptr<TransportManager> transport_manager;

    bool camera_open = false;
    bool microphone_open = false;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);

        capture_manager = m_capture_manager;
        transport_manager = m_transport_manager;

        camera_open = m_camera_open;
        microphone_open = m_microphone_open;

        m_camera_open = false;
        m_microphone_open = false;
        m_current_meeting_id.clear();
        m_local_user_name.clear();
    }

    /*
     * 先停止采集，防止继续产生本地媒体帧。
     */
    if (capture_manager) {
        if (camera_open) {
            capture_manager->CloseCamera();
        }

        if (microphone_open) {
            capture_manager->CloseMic();
        }
    }

    std::vector<std::string> remote_users;

    if (transport_manager) {
        transport_manager->StopPublishCameraVideo();
        transport_manager->StopPublishAudio();

        remote_users = transport_manager->GetSubscribedUsers();

        if (!remote_users.empty()) {
            transport_manager->UnsubscribeGroupUsers(remote_users);
        }
    }

    /*
     * 不在此处调用RenderManager::RemoveUser()。
     * OpenGL资源必须由Qt在对应QOpenGLWidget Context中释放。
     */
    std::vector<std::pair<std::string, UserMediaState>> remaining_states;

    {
        std::lock_guard<std::mutex> lock(m_media_state_mutex);
        remaining_states.reserve(m_user_media_states.size());

        for (const auto& [user_name, state] : m_user_media_states) {
            remaining_states.emplace_back(user_name, state);
        }

        m_user_media_states.clear();
    }

    for (const auto& [user_name, state] : remaining_states) {
        if (state.video_enable) {
            NotifyUserVideoEnable(user_name, false);
        }

        if (state.audio_enable) {
            NotifyUserAudioEnable(user_name, false);
        }
    }
}

// ==================== 远端媒体状态 ====================

void VceEngineImpl::MarkUserVideoReceived(
    const std::string& user_name) {
    bool notify = false;

    {
        std::lock_guard<std::mutex> lock(m_media_state_mutex);
        UserMediaState& state = m_user_media_states[user_name];

        if (!state.video_enable) {
            state.video_enable = true;
            notify = true;
        }
    }

    if (notify) {
        NotifyUserVideoEnable(user_name, true);
    }
}

void VceEngineImpl::MarkUserAudioReceived(
    const std::string& user_name) {
    bool notify = false;

    {
        std::lock_guard<std::mutex> lock(m_media_state_mutex);
        UserMediaState& state = m_user_media_states[user_name];

        if (!state.audio_enable) {
            state.audio_enable = true;
            notify = true;
        }
    }

    if (notify) {
        NotifyUserAudioEnable(user_name, true);
    }
}

void VceEngineImpl::RemoveUserMediaState(
    const std::string& user_name) {
    UserMediaState state;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(m_media_state_mutex);

        const auto iterator = m_user_media_states.find(user_name);
        if (iterator != m_user_media_states.end()) {
            state = iterator->second;
            m_user_media_states.erase(iterator);
            found = true;
        }
    }

    if (!found) {
        return;
    }

    if (state.video_enable) {
        NotifyUserVideoEnable(user_name, false);
    }

    if (state.audio_enable) {
        NotifyUserAudioEnable(user_name, false);
    }
}

// ==================== 观察者管理 ====================

void VceEngineImpl::AddObserver(
    const std::shared_ptr<IMeetingEventObserver>& observer) {
    if (!observer) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_observers_mutex);

    m_observers.erase(
        std::remove_if(
            m_observers.begin(),
            m_observers.end(),
            [](const auto& weak_observer) {
                return weak_observer.expired();
            }),
        m_observers.end());

    for (const auto& weak_observer : m_observers) {
        if (weak_observer.lock() == observer) {
            return;
        }
    }

    m_observers.emplace_back(observer);
}

void VceEngineImpl::RemoveObserver(
    const std::shared_ptr<IMeetingEventObserver>& observer) {
    std::lock_guard<std::mutex> lock(m_observers_mutex);

    m_observers.erase(
        std::remove_if(
            m_observers.begin(),
            m_observers.end(),
            [&observer](const auto& weak_observer) {
                const auto shared_observer = weak_observer.lock();
                return !shared_observer ||
                       shared_observer == observer;
            }),
        m_observers.end());
}

std::vector<std::shared_ptr<IMeetingEventObserver>>
VceEngineImpl::GetObserverSnapshot() {
    std::vector<std::shared_ptr<IMeetingEventObserver>> observers;

    std::lock_guard<std::mutex> lock(m_observers_mutex);

    for (auto iterator = m_observers.begin();
         iterator != m_observers.end();) {
        auto observer = iterator->lock();

        if (!observer) {
            iterator = m_observers.erase(iterator);
            continue;
        }

        observers.emplace_back(std::move(observer));
        ++iterator;
    }

    return observers;
}

// ==================== 观察者通知 ====================

void VceEngineImpl::NotifyUsersJoined(
    const std::vector<std::string>& user_names) {
    if (user_names.empty()) {
        return;
    }

    for (const auto& observer : GetObserverSnapshot()) {
        try {
            observer->OnUserJoined(user_names);
        } catch (const std::exception& exception) {
            LOG_ERROR("Meeting observer OnUserJoined failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR("Meeting observer OnUserJoined failed");
        }
    }
}

void VceEngineImpl::NotifyUsersLeft(
    const std::vector<std::string>& user_names) {
    if (user_names.empty()) {
        return;
    }

    for (const auto& observer : GetObserverSnapshot()) {
        try {
            observer->OnUserLeft(user_names);
        } catch (const std::exception& exception) {
            LOG_ERROR("Meeting observer OnUserLeft failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR("Meeting observer OnUserLeft failed");
        }
    }
}

void VceEngineImpl::NotifyMeetingEnded() {
    for (const auto& observer : GetObserverSnapshot()) {
        try {
            observer->OnMeetingEnded();
        } catch (const std::exception& exception) {
            LOG_ERROR("Meeting observer OnMeetingEnded failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR("Meeting observer OnMeetingEnded failed");
        }
    }
}

void VceEngineImpl::NotifyUserVideoEnable(
    const std::string& user_name,
    bool enable) {
    for (const auto& observer : GetObserverSnapshot()) {
        try {
            observer->OnUserVideoEnable(user_name, enable);
        } catch (const std::exception& exception) {
            LOG_ERROR("Meeting observer video notification failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR("Meeting observer video notification failed");
        }
    }
}

void VceEngineImpl::NotifyUserAudioEnable(
    const std::string& user_name,
    bool enable) {
    for (const auto& observer : GetObserverSnapshot()) {
        try {
            observer->OnUserAudioEnable(user_name, enable);
        } catch (const std::exception& exception) {
            LOG_ERROR("Meeting observer audio notification failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR("Meeting observer audio notification failed");
        }
    }
}

void VceEngineImpl::NotifyTransportStateChanged(
    TransportState state) {
    for (const auto& observer : GetObserverSnapshot()) {
        try {
            observer->OnTransportConnectionStateChanged(state);
        } catch (const std::exception& exception) {
            LOG_ERROR("Meeting observer transport notification failed: {}",
                      exception.what());
        } catch (...) {
            LOG_ERROR("Meeting observer transport notification failed");
        }
    }
}

} // namespace VCE