#include "RenderEngine.h"

#include "audio/AudioMixer.h"
#include "audio/AudioRender.h"
#include "video/GLFrameWork/VideoShader.h"
#include "video/UserContext.h"
#include "utils/logManager.h"

#include <memory>
#include <utility>
#include <vector>

namespace RENDER {

std::unique_ptr<IRenderEngine>
IRenderEngine::CreateRenderEngine() {
    return std::make_unique<RenderEngine>();
}

RenderEngine::RenderEngine() = default;

RenderEngine::~RenderEngine() {
    Uninitialize();
}

bool RenderEngine::Initialize(
    int sample_rate,
    int channels,
    int video_width,
    int video_height) {

    std::lock_guard<std::mutex> state_lock(
        m_state_mutex);

    if (m_initialized.load(
            std::memory_order_acquire)) {

        return true;
    }

    if (sample_rate <= 0 ||
        channels <= 0) {

        LOG_ERROR(
            "Failed to initialize RenderEngine: "
            "invalid audio format, sample_rate={}, channels={}",
            sample_rate,
            channels);

        return false;
    }

    if (video_width <= 0 ||
        video_height <= 0 ||
        (video_width % 2) != 0 ||
        (video_height % 2) != 0) {

        LOG_ERROR(
            "Failed to initialize RenderEngine: "
            "invalid I420 video size {}x{}",
            video_width,
            video_height);

        return false;
    }

    /*
     * 使用局部对象完成初始化。
     *
     * 只有所有步骤都成功后才写入成员变量，
     * 避免RenderEngine停留在初始化一半的状态。
     */
    auto audio_mixer =
        std::make_shared<AudioMixer>();

    if (!audio_mixer->Initialize(
            sample_rate,
            channels)) {

        LOG_ERROR(
            "Failed to initialize AudioMixer");

        return false;
    }

    auto audio_render =
        std::make_unique<AudioRender>();

    /*
     * AudioRender的播放回调从AudioMixer中获取
     * 已经混合完成的Float32 PCM音频。
     *
     * 使用shared_ptr保证音频回调执行期间，
     * AudioMixer对象不会被提前释放。
     */
    audio_render->SetAudioSource(
        audio_mixer);

    if (!audio_render->Initialize(
            sample_rate,
            channels)) {

        LOG_ERROR(
            "Failed to initialize AudioRender");

        audio_render->SetAudioSource(nullptr);
        audio_mixer->Uninitialize();
        return false;
    }

    /*
     * 此时只创建VideoShader的C++对象，
     * 不立即编译OpenGL Shader。
     *
     * RenderEngine::Initialize()可能不在OpenGL渲染线程，
     * 真正的Shader初始化延迟到AddUser()。
     */
    auto shared_shader =
        std::make_shared<VideoShader>();

    m_audio_mixer = std::move(audio_mixer);
    m_audio_render = std::move(audio_render);
    m_video_shader = std::move(shared_shader);

    m_video_width = video_width;
    m_video_height = video_height;

    m_initialized.store(
        true,
        std::memory_order_release);

    LOG_INFO(
        "RenderEngine initialized: "
        "audio={}Hz/{}ch, video={}x{}",
        sample_rate,
        channels,
        video_width,
        video_height);

    return true;
}

void RenderEngine::Uninitialize() {
    std::lock_guard<std::mutex> state_lock(
        m_state_mutex);

    if (!m_initialized.load(
            std::memory_order_acquire)) {

        return;
    }

    /*
     * 首先阻止新的Push、Add、Render等操作进入。
     */
    m_initialized.store(
        false,
        std::memory_order_release);

    /*
     * 先停止音频设备。
     *
     * AudioRender停止后，miniaudio播放回调不会再读取
     * AudioMixer中的混音数据。
     */
    if (m_audio_render) {
        m_audio_render->Uninitialize();
        m_audio_render->SetAudioSource(nullptr);
    }

    /*
     * 快速从用户表中移走所有UserContext。
     *
     * OpenGL资源释放可能需要一定时间，
     * 不应在此期间一直持有m_users_mutex。
     */
    std::vector<std::shared_ptr<UserContext>>
        user_contexts;

    {
        std::lock_guard<std::mutex> users_lock(
            m_users_mutex);

        user_contexts.reserve(
            m_user_contexts.size());

        for (auto& [user_name, context] :
             m_user_contexts) {

            (void)user_name;

            if (context) {
                user_contexts.push_back(context);
            }
        }

        m_user_contexts.clear();
    }

    /*
     * UserContext会依次释放VideoMesh和YUVTexture。
     *
     * 调用Uninitialize()时必须确保当前线程持有兼容的
     * OpenGL Context。最好在销毁窗口Context前，
     * 先通过RemoveUser()逐个释放用户资源。
     */
    for (const auto& context : user_contexts) {
        context->Uninitialize();
    }

    user_contexts.clear();

    /*
     * 所有用户不再使用共享Shader后，
     * 才能释放Shader Program。
     */
    if (m_video_shader) {
        m_video_shader->Uninit();
    }

    /*
     * 停止混音线程并清理所有用户音频缓冲。
     */
    if (m_audio_mixer) {
        m_audio_mixer->Uninitialize();
    }

    m_audio_render.reset();
    m_audio_mixer.reset();
    m_video_shader.reset();

    m_video_width = 0;
    m_video_height = 0;

    LOG_INFO("RenderEngine uninitialized");
}

bool RenderEngine::PushVideoFrame(
    const std::string& user_name,
    const std::shared_ptr<I420Frame>& frame) {

    if (!m_initialized.load(
            std::memory_order_acquire) ||
        user_name.empty() ||
        !frame) {

        return false;
    }

    /*
     * GetUserContext()返回shared_ptr。
     *
     * 即使用户随后从m_user_contexts中移除，
     * 本次调用期间UserContext对象仍然有效。
     */
    const auto context =
        GetUserContext(user_name);

    if (!context) {
        return false;
    }

    /*
     * PushFrame()只把shared_ptr放入缓冲区，
     * 不复制Y、U、V像素数据，也不执行OpenGL操作。
     */
    return context->PushFrame(frame);
}

bool RenderEngine::PushAudioFrame(
    const std::string& user_name,
    const std::shared_ptr<AudioFrame>& frame) {

    if (user_name.empty() || !frame) {
        return false;
    }

    std::shared_ptr<AudioMixer> audio_mixer;

    /*
     * 在状态锁下复制shared_ptr，保证取得对象期间
     * RenderEngine不会同时释放它。
     */
    {
        std::lock_guard<std::mutex> state_lock(
            m_state_mutex);

        if (!m_initialized.load(
                std::memory_order_acquire) ||
            !m_audio_mixer) {

            return false;
        }

        audio_mixer = m_audio_mixer;
    }

    /*
     * 不在m_state_mutex保护下执行音频数据写入，
     * 避免每个音频帧长时间阻塞其他控制操作。
     */
    return audio_mixer->PushAudioData(
        user_name,
        frame);
}

bool RenderEngine::AddUser(const std::string& user_name, bool is_local) {
    if (user_name.empty()) {
        LOG_ERROR(
            "Failed to add render user: "
            "user name is empty");

        return false;
    }

    /*
     * AddUser会创建OpenGL资源。
     *
     * 调用者必须在当前线程绑定该用户对应的
     * OpenGL Context后再调用。
     */
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) || !m_video_shader) {
        return false;
    }

    {
        std::lock_guard<std::mutex> users_lock(m_users_mutex);

        if (m_user_contexts.find(user_name) != m_user_contexts.end()) {
            /*
             * 重复添加同一个用户按成功处理，
             * 保证AddUser具有幂等性。
             */
            return true;
        }
    }

    /*
     * 第一次添加用户时编译并链接共享Shader。
     *
     * VideoShader::Initialize()内部调用gladLoadGL()，
     * 因此此处必须已经存在有效OpenGL Context。
     */
    if (!m_video_shader->IsInitialized() && !m_video_shader->Initialize()) {

        LOG_ERROR(
            "Failed to initialize shared video shader "
            "for user: {}",
            user_name);

        return false;
    }

    auto context = std::make_shared<UserContext>(user_name, is_local);

    /*
     * UserContext内部会创建该用户独立的：
     *
     * VideoMesh；
     * YUVTexture。
     *
     * VideoShader则由所有用户共享。
     */
    if (!context->InitWithShader(
            m_video_shader,
            m_video_width,
            m_video_height)) {

        LOG_ERROR(
            "Failed to initialize UserContext "
            "for user: {}",
            user_name);

        return false;
    }

    {
        std::lock_guard<std::mutex> users_lock(
            m_users_mutex);

        m_user_contexts.emplace(
            user_name,
            context);
    }

    LOG_INFO(
        "Render user added: "
        "user={}, local={}",
        user_name,
        is_local);

    return true;
}

bool RenderEngine::RemoveUser(
    const std::string& user_name) {

    if (user_name.empty()) {
        return false;
    }

    /*
     * RemoveUser会释放OpenGL资源。
     *
     * 调用时必须在当前线程绑定该用户创建资源时所使用的
     * OpenGL Context。
     */
    std::lock_guard<std::mutex> state_lock(
        m_state_mutex);

    if (!m_initialized.load(
            std::memory_order_acquire)) {

        return false;
    }

    /*
     * 音频缓冲与视频用户是否存在相互独立。
     * 即使没有视频UserContext，也尝试清理该用户音频。
     */
    if (m_audio_mixer) {
        m_audio_mixer->RemoveUserBuffer(
            user_name);
    }

    std::shared_ptr<UserContext> context;

    {
        std::lock_guard<std::mutex> users_lock(
            m_users_mutex);

        const auto user =
            m_user_contexts.find(user_name);

        if (user == m_user_contexts.end()) {
            /*
             * 删除不存在的用户按成功处理，
             * 保证RemoveUser具有幂等性。
             */
            return true;
        }

        context = user->second;
        m_user_contexts.erase(user);
    }

    /*
     * 用户已经从查找表移除，新的PushVideoFrame()
     * 不会再找到它。
     *
     * UserContext::Uninitialize()还会关闭帧接收并
     * 清空已经进入缓冲区的帧。
     */
    if (context) {
        context->Uninitialize();
    }

    LOG_INFO(
        "Render user removed: {}",
        user_name);

    return true;
}

bool RenderEngine::RenderUser(const std::string& user_name) {
    if (user_name.empty()) {
        return false;
    }

    /*
     * 持有状态锁直到本次绘制完成，防止绘制过程中
     * RenderEngine、AudioMixer或UserContext被释放。
     */
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_audio_mixer) {
        return false;
    }

    const auto context = GetUserContext(user_name);

    if (!context) {
        return false;
    }

    /*
     * AudioMixer返回音频设备当前消费到的公共媒体时间。
     * 时钟尚未建立时返回std::nullopt，UserContext会降级为
     * 直接渲染；本地预览也会忽略该时钟。
     */
    const auto audio_clock_us =
        m_audio_mixer->GetPlaybackTimestampUs();

    /*
     * UserContext根据音频时钟决定远端视频帧等待、渲染或丢弃。
     * 窗口缓冲区交换仍由上层UI完成。
     */
    return context->RenderFrame(audio_clock_us);
}

bool RenderEngine::UpdateUserVideoSize(
    const std::string& user_name,
    int width,
    int height) {

    if (user_name.empty() ||
        width <= 0 ||
        height <= 0) {

        return false;
    }

    std::lock_guard<std::mutex> state_lock(
        m_state_mutex);

    if (!m_initialized.load(
            std::memory_order_acquire)) {

        return false;
    }

    const auto context =
        GetUserContext(user_name);

    if (!context) {
        return false;
    }

    return context->UpdateVideoSize(
        width,
        height);
}

std::vector<AudioSpeaker>
RenderEngine::GetSpeakerDevices() {
    std::vector<AudioSpeaker> devices;

    std::lock_guard<std::mutex> state_lock(
        m_state_mutex);

    if (!m_initialized.load(
            std::memory_order_acquire) ||
        !m_audio_render) {

        return devices;
    }

    if (!m_audio_render->EnumSpeakers(devices)) {
        devices.clear();
    }

    return devices;
}

bool RenderEngine::UpdateAudioSpeaker(
    const std::string& device_id) {

    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_audio_render ||
        !m_audio_mixer) {
        return false;
    }

    /*
     * SetSpeaker()会向AudioRender工作线程发送设备切换命令，
     * 并等待切换操作完成。切换失败时保留原播放时间线。
     */
    if (!m_audio_render->SetSpeaker(device_id)) {
        return false;
    }

    /*
     * 新播放设备不能继续使用旧设备已经建立的播放位置。
     * 清除旧预混音数据和音频主时钟，等待后续音频重新建立
     * 播放时间线。此期间视频会暂时降级为直接渲染。
     */
    m_audio_mixer->ResetPlaybackTimeline();

    return true;
}

bool RenderEngine::GetCurrentAudioSpeaker(
    std::string& device_id) {

    std::lock_guard<std::mutex> state_lock(
        m_state_mutex);

    if (!m_initialized.load(
            std::memory_order_acquire) ||
        !m_audio_render) {

        device_id.clear();
        return false;
    }

    device_id =
        m_audio_render->GetCurrentSpeakerId();

    return true;
}

std::shared_ptr<UserContext>
RenderEngine::GetUserContext(
    const std::string& user_name) const {

    std::lock_guard<std::mutex> users_lock(
        m_users_mutex);

    const auto user =
        m_user_contexts.find(user_name);

    if (user == m_user_contexts.end()) {
        return nullptr;
    }

    /*
     * 返回shared_ptr而不是裸指针。
     *
     * 即使用户随后从unordered_map中删除，
     * 调用者持有shared_ptr期间对象仍然有效。
     */
    return user->second;
}

} // namespace RENDER