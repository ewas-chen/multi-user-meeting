#include "RenderManager.h"

#include "IRenderEngine.h"
#include "RenderDefine.h"
#include "utils/logManager.h"

#include <utility>

namespace VCE {

RenderManager::RenderManager() = default;

RenderManager::~RenderManager()
{
    Uninit();
}

Result RenderManager::Initialize(int sample_rate, int channels, int video_width, int video_height) {
    if (sample_rate <= 0 || channels <= 0 ||
        video_width <= 0 || video_height <= 0) {
        LOG_ERROR("Invalid RenderManager initialization parameters");
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_initialized.load(std::memory_order_acquire)) {
        return Result::kRet_SUCCESS;
    }

    auto render_engine = RENDER::IRenderEngine::CreateRenderEngine();
    if (!render_engine) {
        LOG_ERROR("Failed to create RenderEngine");
        return Result::kRet_RenderFailedToInit;
    }

    if (!render_engine->Initialize(sample_rate, channels,
                                   video_width, video_height)) {
        LOG_ERROR("Failed to initialize RenderEngine");
        return Result::kRet_RenderFailedToInit;
    }

    m_render_engine = std::move(render_engine);
    m_initialized.store(true, std::memory_order_release);

    LOG_INFO("RenderManager initialized: audio={}Hz/{}ch, video={}x{}",
             sample_rate, channels, video_width, video_height);

    return Result::kRet_SUCCESS;
}

void RenderManager::Uninit()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    m_initialized.store(false, std::memory_order_release);

    if (!m_render_engine) {
        return;
    }

    m_render_engine->Uninitialize();
    m_render_engine.reset();

    LOG_INFO("RenderManager uninitialized");
}

Result RenderManager::AddUser(const std::string& user_name, bool is_local)
{
    if (user_name.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    if (!m_render_engine->AddUser(user_name, is_local)) {
        LOG_ERROR("Failed to add render user: user={}, local={}",
                  user_name, is_local);

        return Result::kRet_RenderFailedToAddUser;
    }

    LOG_INFO("Render user added: user={}, local={}", user_name, is_local);
    return Result::kRet_SUCCESS;
}

Result RenderManager::RemoveUser(const std::string& user_name)
{
    if (user_name.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    if (!m_render_engine->RemoveUser(user_name)) {
        LOG_ERROR("Failed to remove render user: {}", user_name);
        return Result::kRet_RenderFailedToRemoveUser;
    }

    LOG_INFO("Render user removed: {}", user_name);
    return Result::kRet_SUCCESS;
}

Result RenderManager::RenderUserFrame(const std::string& user_name)
{
    if (user_name.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    if (!m_render_engine->RenderUser(user_name)) {
        return Result::kRet_RenderFailedToUpdateVideo;
    }

    return Result::kRet_SUCCESS;
}

Result RenderManager::UpdateUserVideoSize(const std::string& user_name, int width, int height) {
    if (user_name.empty() || width <= 0 || height <= 0) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) || !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    if (!m_render_engine->UpdateUserVideoSize(user_name, width, height)) {
        LOG_ERROR("Failed to update user video size: user={}, size={}x{}",
                  user_name, width, height);

        return Result::kRet_RenderFailedToResize;
    }

    return Result::kRet_SUCCESS;
}

Result RenderManager::PushVideoFrame(const std::string& user_name, const std::shared_ptr<I420Frame>& frame) {
    if (user_name.empty() || !frame || !frame->IsValid()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    /*
     * RenderEngine内部已经具有按用户管理的视频帧队列。
     * 这里直接传递shared_ptr，不增加额外队列，也不复制I420数据。
     */
    if (!m_render_engine->PushVideoFrame(user_name, frame)) {
        return Result::kRet_RenderFailedToUpdateVideo;
    }

    return Result::kRet_SUCCESS;
}

Result RenderManager::PushAudioFrame(const std::string& user_name, const std::shared_ptr<AudioFrame>& frame) {
    if (user_name.empty() || !frame || !frame->IsValid()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    /*
     * AudioMixer内部已经管理每个用户的音频环形缓冲区。
     * Manager只负责直接转发公共AudioFrame。
     */
    if (!m_render_engine->PushAudioFrame(user_name, frame)) {
        return Result::kRet_RenderFailedToUpdateAudio;
    }

    return Result::kRet_SUCCESS;
}

Result RenderManager::GetAudioSpeakers(std::vector<SpeakerDeviceInfo>& speakers) {
    speakers.clear();

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    const auto render_speakers = m_render_engine->GetSpeakerDevices();
    speakers.reserve(render_speakers.size());

    for (const auto& render_speaker : render_speakers) {
        SpeakerDeviceInfo speaker;
        speaker.name = render_speaker.name;
        speaker.id = render_speaker.device_id;
        speaker.is_default = render_speaker.is_default;
        speakers.emplace_back(std::move(speaker));
    }

    return Result::kRet_SUCCESS;
}

Result RenderManager::UpdateAudioSpeaker(const std::string& speaker_id) {
    if (speaker_id.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    if (!m_render_engine->UpdateAudioSpeaker(speaker_id)) {
        LOG_ERROR("Failed to update audio speaker: {}", speaker_id);
        return Result::kRet_RenderFailedToSetSpeaker;
    }

    return Result::kRet_SUCCESS;
}

Result RenderManager::GetCurrentAudioSpeaker(std::string& speaker_id)
{
    speaker_id.clear();

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_render_engine) {
        return Result::kRet_RenderNotInitialized;
    }

    if (!m_render_engine->GetCurrentAudioSpeaker(speaker_id)) {
        LOG_ERROR("Failed to get current audio speaker");
        return Result::kRet_RenderFailedToGetSpeaker;
    }

    return Result::kRet_SUCCESS;
}

} // namespace VCE