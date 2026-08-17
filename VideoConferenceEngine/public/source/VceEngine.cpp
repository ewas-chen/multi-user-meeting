#include "VceEngine.h"

#include "VceEngineImpl.h"

#include <utility>

namespace VCE {

std::shared_ptr<VceEngine> VceEngine::m_instance;
std::mutex VceEngine::m_instance_mutex;

std::shared_ptr<VceEngine> VceEngine::GetInstance() {
    std::lock_guard<std::mutex> lock(m_instance_mutex);

    if (!m_instance) {
        /*
         * 构造函数是private，不能直接使用make_shared。
         */
        m_instance =
            std::shared_ptr<VceEngine>(
                new VceEngine());
    }

    return m_instance;
}

void VceEngine::ReleaseInstance() {
    std::shared_ptr<VceEngine> released_instance;

    {
        std::lock_guard<std::mutex> lock(m_instance_mutex);

        /*
         * 将实例移出静态成员，并在释放单例锁后销毁。
         * 避免析构过程中再次访问VceEngine单例造成死锁。
         */
        released_instance = std::move(m_instance);
    }
}

VceEngine::VceEngine()
    : m_impl(std::make_shared<VceEngineImpl>()) {
}

VceEngine::~VceEngine() = default;

// ==================== 引擎生命周期 ====================

Result VceEngine::InitSubModel(
    const EngineConfig& config) {
    return m_impl->InitSubModel(config);
}

Result VceEngine::UninitSubModel() {
    return m_impl->UninitSubModel();
}

// ==================== 用户服务 ====================

Result VceEngine::RegisterUser(
    const std::string& user_name,
    const std::string& password) {
    return m_impl->RegisterUser(
        user_name,
        password);
}

Result VceEngine::LoginUser(
    const std::string& user_name,
    const std::string& password) {
    return m_impl->LoginUser(
        user_name,
        password);
}

// ==================== 会议管理 ====================

Result VceEngine::CreateMeeting(
    const CreateMeetingInfo& request,
    CreateMeetingResponse& response) {
    return m_impl->CreateMeeting(
        request,
        response);
}

Result VceEngine::JoinMeeting(
    const MeetingBriefInfo& request,
    JoinMeetingResponse& response) {
    return m_impl->JoinMeeting(
        request,
        response);
}

Result VceEngine::LeaveMeeting(
    const MeetingBriefInfo& request) {
    return m_impl->LeaveMeeting(request);
}

Result VceEngine::EndMeeting(
    const MeetingBriefInfo& request) {
    return m_impl->EndMeeting(request);
}

Result VceEngine::GetMeetingList(
    const GetMeetingListRequest& request,
    GetMeetingListResponse& response) {
    return m_impl->GetMeetingList(
        request,
        response);
}

// ==================== 采集设备管理 ====================

Result VceEngine::GetCameraDevices(
    std::vector<CameraDeviceInfo>& devices) {
    return m_impl->GetCameraDevices(devices);
}

Result VceEngine::GetMicrophoneDevices(
    std::vector<MicDeviceInfo>& devices) {
    return m_impl->GetMicrophoneDevices(devices);
}

Result VceEngine::GetCurrentCameraDeviceId(
    std::string& camera_device_id) {
    return m_impl->GetCurrentCameraDeviceId(
        camera_device_id);
}

Result VceEngine::GetCurrentMicrophoneDeviceId(
    std::string& microphone_device_id) {
    return m_impl->GetCurrentMicrophoneDeviceId(
        microphone_device_id);
}

Result VceEngine::UpdateCameraDevice(
    const std::string& camera_device_id) {
    return m_impl->UpdateCameraDevice(
        camera_device_id);
}

Result VceEngine::UpdateMicrophoneDevice(
    const std::string& microphone_device_id) {
    return m_impl->UpdateMicrophoneDevice(
        microphone_device_id);
}

Result VceEngine::OpenCamera() {
    return m_impl->OpenCamera();
}

Result VceEngine::CloseCamera() {
    return m_impl->CloseCamera();
}

Result VceEngine::OpenMic() {
    return m_impl->OpenMic();
}

Result VceEngine::CloseMic() {
    return m_impl->CloseMic();
}

// ==================== 渲染管理 ====================

Result VceEngine::AddUser(
    const std::string& user_name,
    bool is_local) {
    return m_impl->AddUser(
        user_name,
        is_local);
}

Result VceEngine::RemoveUser(
    const std::string& user_name) {
    return m_impl->RemoveUser(user_name);
}

Result VceEngine::RenderUser(
    const std::string& user_name) {
    return m_impl->RenderUser(user_name);
}

Result VceEngine::UpdateUserVideoSize(
    const std::string& user_name,
    int width,
    int height) {
    return m_impl->UpdateUserVideoSize(
        user_name,
        width,
        height);
}

Result VceEngine::GetAudioSpeakers(
    std::vector<SpeakerDeviceInfo>& speakers) {
    return m_impl->GetAudioSpeakers(speakers);
}

Result VceEngine::UpdateAudioSpeaker(
    const std::string& speaker_id) {
    return m_impl->UpdateAudioSpeaker(speaker_id);
}

Result VceEngine::GetCurrentAudioSpeaker(
    std::string& speaker_id) {
    return m_impl->GetCurrentAudioSpeaker(
        speaker_id);
}

// ==================== 会议事件观察者 ====================

void VceEngine::AddObserver(const std::shared_ptr<IMeetingEventObserver>& observer) {
    m_impl->AddObserver(observer);
}

void VceEngine::RemoveObserver(const std::shared_ptr<IMeetingEventObserver>& observer) {
    m_impl->RemoveObserver(observer);
}

} // namespace VCE