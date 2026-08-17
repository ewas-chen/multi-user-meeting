#include "CaptureEngine.h"
#include "utils/logManager.h"
#include "ObsCaptureConstants.h"
#include "../source/CameraSource.h"
#include "../source/MicSource.h"
#include "../property/CameraSourceProperty.h"
#include "../property/MicSourceProperty.h"

#include <exception>
#include <utility>
#include <cstring>
#include <memory>

#include <obs/obs-nix-platform.h>

namespace {
constexpr const char* kObsLocale = "en-US";

// 插件动态库路径，
constexpr const char* kObsPluginBinaryPath =
    "/usr/lib/x86_64-linux-gnu/obs-plugins";

// 插件的数据、语言文件和配置资源路径
constexpr const char* kObsPluginDataPath =
    "/usr/share/obs/obs-plugins/%module%";

constexpr const char* kObsGraphicsModule =
    "libobs-opengl";

bool HasObsInputType(const char* expected_id) {
    if (!expected_id) {
        return false;
    }

    const char* input_id = nullptr;
    for (size_t index = 0; obs_enum_input_types(index, &input_id); index++) {
        if (input_id && std::strcmp(input_id, expected_id) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace CAPTURE {

std::unique_ptr<ICaptureEngine> ICaptureEngine::Create() {
    return std::make_unique<CaptureEngine>();
}

CaptureEngine::~CaptureEngine() {
    if (m_initialized.load()) {
        if (!UnInit()) {
            LOG_ERROR("UnInit fail");
        }
    }
}

bool CaptureEngine::Init(int sample_rate, int channels, int width, int height, int fps) {
    if (m_initialized.load()) {
        LOG_ERROR("already inited");
        return true;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);
    if (sample_rate <= 0 || (channels != 1 && channels != 2) || width <= 0 || height <= 0 || fps <= 0) {
        LOG_ERROR("init args error");
        return false;
    }

    if (width % 2 || height % 2) {
        LOG_ERROR("width:{}, height:{}", width, height);
        return false;
    }

    // 防止残余obs状态信息
    if (m_obs_acquired) {
        CleanupObsState();
    }

    // 当前程序运行于 Wayland，但使用可用的 XWayland 创建 OBS OpenGL 上下文。
    obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);

    // 启动OBS
    if (!obs_startup(kObsLocale, nullptr, nullptr)) {
        LOG_ERROR("obs start fail");
        return false;
    }

    m_obs_acquired = true;
    LOG_INFO("OBS start, version:{}", obs_get_version());

    // 添加插件搜索路径
    // obs_add_module_path(kObsPluginBinaryPath, kObsPluginDataPath);

    // 加载路径下所有插件
    // obs_load_all_modules() 加载了 DeckLink 等依赖 OBS 前端和 Qt 的插件，而当前程序没有 QApplication，所以不能全部加载

    // obs_load_all_modules();

    // 手动指定并加载摄像头、麦克风两个 OBS 插件
    struct CaptureModulePath {
        const char* binary_path;
        const char* data_path;
    };

    const CaptureModulePath capture_modules[] = {
        {
            "/usr/lib/x86_64-linux-gnu/obs-plugins/linux-v4l2.so",
            "/usr/share/obs/obs-plugins/linux-v4l2"
        },
        {
            "/usr/lib/x86_64-linux-gnu/obs-plugins/linux-pulseaudio.so",
            "/usr/share/obs/obs-plugins/linux-pulseaudio" // 插件的数据和语言资源目录
        }
    };

    for (const auto& module_path : capture_modules) {
        obs_module_t* module = nullptr;

        // 根据明确路径打开插件动态库, 没有正式初始化插件
        const int result = obs_open_module(&module, module_path.binary_path, module_path.data_path);

        if (result != MODULE_SUCCESS) {
            LOG_ERROR(
                "Failed to open OBS module: {}, result={}",
                module_path.binary_path, result
            );

            // 执行当前Init已有的失败清理逻辑
            return false;
        }

        if (!obs_init_module(module)) {
            LOG_ERROR(
                "Failed to initialize OBS module: {}",
                module_path.binary_path
            );

            // 执行当前Init已有的失败清理逻辑
            return false;
        }
    }

    /*
        完成模块加载阶段
        这个函数通知所有成功加载的模块：
        其他模块现在已经全部加载完毕，可以执行依赖其他模块的后续初始化
    */
    obs_post_load_modules();

    // 将所有已经加载的模块写入 OBS 日志
    obs_log_loaded_modules();

    if (!HasObsInputType(kCameraSourceId)) {
        LOG_ERROR("Required OBS camera input {} is unavailable", kCameraSourceId);
        CleanupObsState();
        return false;
    }

    if (!HasObsInputType(kMicSourceId)) {
        LOG_ERROR("Required OBS microphone input {} is unavailable", kMicSourceId);
        CleanupObsState();
        return false;
    }
    LOG_INFO("Required OBS input modules loaded successfully");

    // 描述 OBS 的全局视频处理和输出参数
    obs_video_info video_info{};
    video_info.graphics_module = kObsGraphicsModule; // OBS 的图形渲染后端

    // OBS 场景的基础合成画布大小(Source 最终都会合成到这个大小的画布上)
    video_info.base_height = static_cast<uint32_t>(height);
    video_info.base_width = static_cast<uint32_t>(width);

    // OBS 最终向编码器或原始视频回调输出的分辨率
    video_info.output_height = static_cast<uint32_t>(height);
    video_info.output_width = static_cast<uint32_t>(width);

    // 帧率 = fps_num / fps_den
    video_info.fps_num = static_cast<uint32_t>(fps);
    video_info.fps_den = 1;

    video_info.output_format = VIDEO_FORMAT_I420; // OBS 最终视频输出使用 I420
    video_info.colorspace = VIDEO_CS_709; // 表示使用 BT.709 的 RGB->YUV 色彩转换规则，一般用于高清视频
    video_info.range = VIDEO_RANGE_FULL; // 色彩范围, 表示使用全范围 YUV(全范围和有限范围的数值解释不同。后续编码器、播放器和网络对端必须采用一致的范围设置)
    video_info.adapter = 0; // 使用索引为 0 的图形适配器(GPU)
    video_info.gpu_conversion = true; // 尽量使用 GPU Shader 完成输出色彩格式转换

    const int video_result = obs_reset_video(&video_info); // 启动视频子系统
    if (video_result != OBS_VIDEO_SUCCESS) {
        LOG_ERROR("Failed to initialize OBS video, result: {}", video_result);
        CleanupObsState();
        return false;
    }

    // 配置 OBS 音频系统
    obs_audio_info audio_info{};
    audio_info.samples_per_sec = static_cast<uint32_t>(sample_rate);
    audio_info.speakers = channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
    if (!obs_reset_audio(&audio_info)) {
        LOG_ERROR("Failed to initialize OBS audio");
        CleanupObsState();
        return false;
    }

    OBSSceneAutoRelease scene = obs_scene_create("capture_default_scene");
    if (!scene) {
        LOG_ERROR("Failed to create default scene source");
        CleanupObsState();
        return false;
    }

    m_default_scene = scene.Get();

    // 将这个场景产生的画面，设置为 OBS 第 0 号主输出通道的视频源(obs_source_t:真正的视频源,这里代表整个场景合成结果的源)
    obs_source_t* scene_source = obs_scene_get_source(m_default_scene);
    if (!scene_source) {
        LOG_ERROR("Failed to get default scene source");
        CleanupObsState();
        return false;
    }

    obs_set_output_source(0, scene_source);
    m_sample_rate = sample_rate;
    m_channels = channels;
    m_initialized.store(true);

    LOG_INFO("CaptureEngine inited");

    return true;
}

bool CaptureEngine::UnInit() {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);
    if (!m_obs_acquired) {
        m_initialized.store(false, std::memory_order_release);
        return true;
    }
    CleanupObsState();
    return true;
}

bool CaptureEngine::IsInitialized() const noexcept {
    return m_initialized.load(std::memory_order_acquire);
}

void CaptureEngine::UnregisterRawCallbacks() noexcept {
    bool remove_video_callback = false;
    bool remove_audio_callback = false;

    {
        std::lock_guard<std::mutex> callback_lock(m_callback_mutex);

        m_video_callback = nullptr;
        m_audio_callback = nullptr;

        remove_video_callback = m_video_callback_registered;
        remove_audio_callback = m_audio_callback_registered;

        m_video_callback_registered = false;
        m_audio_callback_registered = false;
    }

    if (remove_video_callback) {
        obs_remove_raw_video_callback(&CaptureEngine::OnRawVideoData, this);
    }

    if (remove_audio_callback) {
        /*
            表示音频混音索引
            注册回调时, 最后一个参数 this 会原样传给回调的 param, 这样一个静态回调函数就可以找到对应的 CaptureEngine 对象
        */ 
        obs_remove_raw_audio_callback(0, &CaptureEngine::OnRawAudioData, this);
    }
}

void CaptureEngine::RemoveAllSources() noexcept {
    if (m_default_scene) {

        // obs_scene_item：某个 Source 被放入场景后产生的“场景项”
        for (const auto& [name, scource] : m_sources) {
            obs_scene_item *scene_item = obs_scene_find_source(m_default_scene, name.c_str());
            if (scene_item) {
                obs_sceneitem_remove(scene_item);
            }
        }
    }
    m_sources.clear();
}

void CaptureEngine::CleanupObsState() noexcept {
    m_initialized.store(false);
    UnregisterRawCallbacks();
    RemoveAllSources();

    if (m_obs_acquired) {
         /*
         * output source 会持有默认场景的引用，
         * 必须先解除 output source。
         */
        obs_set_output_source(0, nullptr);
        m_default_scene = nullptr;
        obs_shutdown();
        m_obs_acquired = false;
    }

    m_sample_rate = 48000;
    m_channels = 2;
}

// callback空表示移除
void CaptureEngine::RegisterVideoCallback(VideoDataCallback callback) {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    const bool has_callback = static_cast<bool>(callback);
    if (has_callback && !m_initialized.load()) {
        LOG_ERROR("CaptureEngine is not initialized");
        return;
    }

    bool add_callback = false;
    bool remove_callback = false;

    {
        std::lock_guard<std::mutex> callback_lock(m_callback_mutex);
        m_video_callback = std::move(callback);

        if (has_callback && !m_video_callback_registered) {
            m_video_callback_registered = true;
            add_callback = true;
        } else if (!has_callback && m_video_callback_registered) {
            m_video_callback_registered = false;
            remove_callback = true;
        }
    }

    if (add_callback) {
        /*
         * nullptr 表示不额外转换。
         * Init() 已经将全局输出格式设置为 I420。
         */
        obs_add_raw_video_callback(nullptr, &CaptureEngine::OnRawVideoData, this);
        LOG_INFO("OBS raw video callback registered");            
    }

    if (remove_callback) {
        obs_remove_raw_video_callback(&CaptureEngine::OnRawVideoData, this);
        LOG_INFO("OBS raw video callback removed");
    }
}

// callback是上层回调，真正给OBS的回调是OnRawAudioData，它里面会调用
void CaptureEngine::RegisterAudioCallback(AudioDataCallback callback) {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    const bool has_callback = static_cast<bool>(callback);

    if (has_callback && !m_initialized.load()) {
        LOG_ERROR("CaptureEngine is not initialized");
        return;
    }

    bool add_callback = false;
    bool remove_callback = false;

    {
        std::lock_guard<std::mutex> callback_lock(m_callback_mutex);
        m_audio_callback = std::move(callback);

        if (has_callback && !m_audio_callback_registered) {
            m_audio_callback_registered = true;
            add_callback = true;
        } else if (!has_callback && m_audio_callback_registered) {
            m_audio_callback_registered = false;
            remove_callback = true;
        }
    }

    if (add_callback) {
        audio_convert_info conversion{};
        conversion.format = AUDIO_FORMAT_FLOAT;
        conversion.samples_per_sec = static_cast<std::uint32_t>(m_sample_rate);
        conversion.speakers = m_channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;

        obs_add_raw_audio_callback(0, &conversion, &CaptureEngine::OnRawAudioData, this);
        LOG_INFO("OBS raw audio callback registered");
    }

    if (remove_callback) {
        obs_remove_raw_audio_callback(0, &CaptureEngine::OnRawAudioData, this);
        LOG_INFO("OBS raw audio callback removed");
    }
}

std::shared_ptr<ISource> CaptureEngine::CreateSource(CaptureSourceType type, const std::string& name) {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (!m_initialized.load()) {
        LOG_ERROR("Failed to create source {}: "
                  "CaptureEngine is not initialized",
                  name);
        return nullptr;
    }

    if (name.empty()) {
        LOG_ERROR("Failed to create source: source name is empty");
        return nullptr;
    }

    auto existing = m_sources.find(name);
    if (existing != m_sources.end()) {
        LOG_INFO("Source {} already exists", name);
        return existing->second;
    }

    std::shared_ptr<ISource> source;
    switch (type) {
        case CaptureSourceType::kCST_Camera:
            source = CreateCameraSource(name);
            break;

        case CaptureSourceType::kCST_Mic:
            source = CreateMicSource(name);
            break;

        default:
            LOG_ERROR("Failed to create source {}: unknown source type {}",
                    name, static_cast<int>(type));
            return nullptr;
    }

    if (!source) {
        return nullptr;
    }

    m_sources.emplace(name, source);
    return source;
}

bool CaptureEngine::RemoveSource(const std::string& name) {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (!m_initialized.load()) {
        LOG_ERROR("Failed to remove source {}: "
                  "CaptureEngine is not initialized",
                  name);
        return false;
    }

    auto source_iterator = m_sources.find(name);

    if (source_iterator == m_sources.end()) {
        LOG_ERROR("Failed to remove source {}: source was not found", name);
        return false;
    }

    if (m_default_scene) {
        obs_sceneitem_t* scene_item = obs_scene_find_source(m_default_scene.Get(), name.c_str());
        if (scene_item) {
            obs_sceneitem_remove(scene_item);
        }            
    }
    m_sources.erase(source_iterator);
    return true;
}

std::shared_ptr<ISource> CaptureEngine::GetSource(const std::string& name) {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);
    const auto source_iterator = m_sources.find(name);

    if (source_iterator == m_sources.end()) {
        return nullptr;
    }

    return source_iterator->second;
}

std::vector<std::shared_ptr<ISource>> CaptureEngine::GetAllSources() {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);
    std::vector<std::shared_ptr<ISource>> sources;
    sources.reserve(m_sources.size());

    for (const auto& entry : m_sources) {
        sources.push_back(entry.second);
    }

    return sources;
}

std::vector<std::shared_ptr<ISource>> CaptureEngine::GetActiveSources() {
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    std::vector<std::shared_ptr<ISource>> active_sources;
    if (!m_default_scene) {
        return active_sources;
    }

    struct EnumContext {
        CaptureEngine* engine;
        std::vector<std::shared_ptr<ISource>>* result;
    };

    EnumContext context{this, &active_sources};

    // 遍历指定场景中的场景项，并对每一项调用一次回调
    obs_scene_enum_items(m_default_scene.Get(), 
        [](obs_scene_t*, obs_sceneitem_t* scene_item, void* parameter) -> bool{
            auto* context = static_cast<EnumContext*>(parameter);
            if (!context || !scene_item) {
                return true; // 继续枚举下一个场景项
            }

            // 获取当前场景项的可见状态
            if (!obs_sceneitem_visible(scene_item)) {
                return true;
            }

            // 从场景项取得 Source
            obs_source_t* obs_source = obs_sceneitem_get_source(scene_item);
            if (!obs_source) {
                return true;
            }

            const char* source_name = obs_source_get_name(obs_source);
            if (!source_name) {
                return true;
            }

            const auto source_iterator = context->engine->m_sources.find(source_name);
            if (source_iterator != context->engine->m_sources.end()) {
                context->result->push_back(source_iterator->second);
            }

            return true;
        },
        &context
    );
    return active_sources;
}

void CaptureEngine::OnRawVideoData(void* param, struct video_data* frame) {
    if (!param || !frame) {
        return;
    }

    auto* engine = static_cast<CaptureEngine*>(param);
    try {
        VideoDataCallback callback;

        {
            std::lock_guard<std::mutex> callback_lock(engine->m_callback_mutex);
            callback = engine->m_video_callback;
        }
        if (!callback) {
            return;
        }

        // 视频帧转化为自定义的I420
        auto converted_frame = engine->ConvertVideoData(frame);
        if (converted_frame) {
            callback(converted_frame); // 不持有 m_callback_mutex 执行用户回调
        }
    } catch(const std::exception& e) {
        LOG_ERROR("Video callback exception: {}",e.what());
    } catch (...) {
        LOG_ERROR("Unknown video callback exception");
    }
}

void CaptureEngine::OnRawAudioData(void* param, std::size_t mix_index, struct audio_data* frame) {
    (void)mix_index;

    if (!param || !frame) {
        return;
    }

    auto* engine = static_cast<CaptureEngine*>(param);
    try {
        AudioDataCallback  callback;

        {
            std::lock_guard<std::mutex> callback_lock(engine->m_callback_mutex);
            callback = engine->m_audio_callback;
        }
        if (!callback) {
            return;
        }

        // 视频帧转化为自定义的I420
        auto converted_frame = engine->ConvertAudioData(frame);
        if (converted_frame) {
            callback(converted_frame); // 不持有 m_callback_mutex 执行用户回调
        }
    } catch(const std::exception& e) {
        LOG_ERROR("Audio callback exception: {}",e.what());
    } catch (...) {
        LOG_ERROR("Unknown video callback exception");
    }
}

std::shared_ptr<ISource>
CaptureEngine::CreateCameraSource(const std::string& name) {
    auto property_interface = ICameraSourceProperty::Create();
    if (!property_interface) {
        LOG_ERROR("Failed to create camera property for {}", name);
        return nullptr;
    }

    // 转换为具体子类指针
    auto property = std::dynamic_pointer_cast<CameraSourceProperty>(property_interface);
    if (!property) {
        LOG_ERROR("Failed to convert camera property for {}",name);
        return nullptr;
    }

    auto source = std::make_shared<CameraSource>();
    // 此时没有关联具体source设备，property_interface默认只有devicetype，创建默认setting和obs_source
    if (!source->Init(name, property)) {
        LOG_ERROR("Failed to initialize camera source {}", name);
        return nullptr;
    }

    obs_sceneitem_t* scene_item = obs_scene_add(m_default_scene.Get(), source->GetObsSource());
    if (!scene_item) {
        LOG_ERROR("Failed to add camera source '{}' to scene", name);
        return nullptr;
    }
        
    // 获取 Init() 中通过 obs_reset_video() 设置的全局视频输出配置，不是摄像头配置
    obs_video_info video_info{};
    if (obs_get_video_info(&video_info)) {
        vec2 bounds{}; // 将摄像头显示区域设为整个 OBS 画布
        bounds.x = static_cast<float>(video_info.base_width);
            
        bounds.y = static_cast<float>(video_info.base_height);

        // 保持摄像头原始宽高比，完整缩放到画布内，可能出现黑边
        obs_sceneitem_set_bounds_type(scene_item, OBS_BOUNDS_SCALE_INNER);

        // 将画面居中
        obs_sceneitem_set_bounds_alignment(scene_item, OBS_ALIGN_CENTER);

        obs_sceneitem_set_bounds(scene_item, &bounds);
    }

    return source;
}

std::shared_ptr<ISource>
CaptureEngine::CreateMicSource(const std::string& name) {
    auto property_interface = IMicSourceProperty::Create();

    if (!property_interface) {
        LOG_ERROR("Failed to create microphone property for '{}'", name);
        return nullptr;
    }

    auto property = std::dynamic_pointer_cast<MicSourceProperty>(property_interface);
    if (!property) {
        LOG_ERROR("Failed to convert microphone property for '{}'", name);
        return nullptr;
    }

    auto source = std::make_shared<MicSource>();
    // property为默认配置
    if (!source->Init(name, property)) {
        LOG_ERROR("Failed to initialize microphone source '{}'", name);
        return nullptr;
    }

    // 关闭麦克风的本地监听,也就是说麦克风声音会进入 OBS 的采集和混音链路，但不会从本机扬声器再次播放，避免产生回声或啸叫
    obs_source_set_monitoring_type(source->GetObsSource(), OBS_MONITORING_TYPE_NONE);

    // 把麦克风 OBS 源添加到默认场景中; scene_item：表示该源在场景中的项目，可用于删除、显示或隐藏等操作
    obs_sceneitem_t* scene_item = obs_scene_add(m_default_scene.Get(), source->GetObsSource());
    if (!scene_item) {
        LOG_ERROR("Failed to add microphone source '{}' to scene", name);
        return nullptr;
    }

    return source;        
}

std::shared_ptr<I420Frame>
CaptureEngine::ConvertVideoData(const struct video_data* obs_frame) const {
    if (!obs_frame || !obs_frame->data[0] || !obs_frame->data[1] || !obs_frame->data[2]) {
        return nullptr;
    }

    obs_video_info video_info{};
    if (!obs_get_video_info(&video_info)) {
        return nullptr;
    }

    const std::size_t width = video_info.output_width;
    const std::size_t height = video_info.output_height;

    if (width == 0 || height == 0 || (width & 1U) != 0 || (height & 1U) != 0) {
        return nullptr;
    }

    const std::size_t chroma_width = width / 2;
    const std::size_t chroma_height = height / 2;

    if (obs_frame->linesize[0] < width || obs_frame->linesize[1] < chroma_width ||
        obs_frame->linesize[2] < chroma_width) {
        LOG_ERROR("Invalid I420 frame linesize");
        return nullptr;
    }

    const std::size_t y_size = width * height;
    const std::size_t uv_size = chroma_width * chroma_height;
        
    auto frame = std::make_shared<I420Frame>();
    
    frame->width = static_cast<decltype(frame->width)>(width);
    frame->height = static_cast<decltype(frame->height)>(height);
        
    frame->timestamp_us = static_cast<std::int64_t>(obs_frame->timestamp / 1000);
        
    frame->data[0] = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[y_size]);
    frame->data[1] = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[uv_size]);
    frame->data[2] = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[uv_size]);
         
    for (std::size_t row = 0; row < height; ++row) {
        std::memcpy(
            frame->data[0].get() + row * width,
            obs_frame->data[0] + row * obs_frame->linesize[0],
            width);
    }
         
    for (std::size_t row = 0; row < chroma_height; ++row) {
        std::memcpy(
            frame->data[1].get() + row * chroma_width,
            obs_frame->data[1] + row * obs_frame->linesize[1],
            chroma_width);

        std::memcpy(
            frame->data[2].get() + row * chroma_width,
            obs_frame->data[2] + row * obs_frame->linesize[2],
            chroma_width);
    }

    return frame;
}

std::shared_ptr<AudioFrame>
CaptureEngine::ConvertAudioData(const struct audio_data* obs_frame) const {
    if (!obs_frame || !obs_frame->data[0] || obs_frame->frames == 0 ||
        m_sample_rate <= 0 || (m_channels != 1 && m_channels != 2)) {
        return nullptr;
    }

    const std::size_t data_size =
        static_cast<std::size_t>(obs_frame->frames) *
        static_cast<std::size_t>(m_channels) *
        sizeof(float);

    auto frame = std::make_shared<AudioFrame>();

    frame->samples = static_cast<decltype(frame->samples)>(obs_frame->frames);

    frame->channels = m_channels;
    frame->sample_rate = m_sample_rate;

    frame->timestamp_us = static_cast<std::int64_t>(obs_frame->timestamp / 1000);

    frame->data = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[data_size]);

    std::memcpy(frame->data.get(), obs_frame->data[0], data_size);

    return frame;
}



} // namespace