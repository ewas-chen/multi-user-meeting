#include "CaptureManager.h"

#include "ICameraSource.h"
#include "ICameraSourceProperty.h"
#include "IMicSource.h"
#include "IMicSourceProperty.h"
#include "utils/logManager.h"

#include <exception>
#include <utility>
#include <algorithm>

namespace VCE {

CaptureManager::CaptureManager() = default;

CaptureManager::~CaptureManager()
{
    Uninit();
}

Result CaptureManager::Initialize(
    int sample_rate,
    int channels,
    int video_width,
    int video_height,
    int video_fps)
{
    if (sample_rate <= 0 || channels <= 0 ||
        video_width <= 0 || video_height <= 0 ||
        video_fps <= 0) {
        LOG_ERROR("Invalid CaptureManager initialization parameters");
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_initialized.load(std::memory_order_acquire)) {
        return Result::kRet_SUCCESS;
    }

    auto capture_engine = CAPTURE::ICaptureEngine::Create();
    if (!capture_engine) {
        LOG_ERROR("Failed to create CaptureEngine");
        return Result::kRet_CaptureFailedToInit;
    }

    if (!capture_engine->Init(
            sample_rate,
            channels,
            video_width,
            video_height,
            video_fps)) {
        LOG_ERROR("Failed to initialize CaptureEngine");
        return Result::kRet_CaptureFailedToInit;
    }

    m_capture_engine = std::move(capture_engine);
    m_initialized.store(true, std::memory_order_release);

    LOG_INFO(
        "CaptureManager initialized: audio={}Hz/{}ch, video={}x{}@{}fps",
        sample_rate,
        channels,
        video_width,
        video_height,
        video_fps);

    return Result::kRet_SUCCESS;
}

void CaptureManager::Uninit()
{
    /*
     * 先解除VceEngineImpl回调。
     * 即使采集模块仍有一帧正在回调，也不会再进入上层调度对象。
     */
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_capture_callback.reset();
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    m_initialized.store(false, std::memory_order_release);

    if (!m_capture_engine) {
        m_source_id_info_map.clear();
        m_camera_source_id = -1;
        m_microphone_source_id = -1;
        return;
    }

    /*
     * 先注销帧回调，防止删除采集源时还有新的帧进入Manager。
     */
    m_capture_engine->RegisterVideoCallback({});
    m_capture_engine->RegisterAudioCallback({});

    for (const auto& source_info : m_source_id_info_map) {
        m_capture_engine->RemoveSource(
            source_info.second.first);
    }

    m_source_id_info_map.clear();
    m_camera_source_id = -1;
    m_microphone_source_id = -1;

    m_capture_engine->UnInit();
    m_capture_engine.reset();

    LOG_INFO("CaptureManager uninitialized");
}

Result CaptureManager::GetCameraDevices(
    std::vector<CameraDeviceInfo>& devices)
{
    devices.clear();

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    std::shared_ptr<CAPTURE::ICameraSourceProperty> property;
    int temporary_source_id = -1;

    if (m_camera_source_id != -1) {
        /*
         * 摄像头已经打开，直接使用当前采集源持有的属性。
         */
        property = GetCameraProperty();
    } else {
        /*
         * CameraSourceProperty需要关联真实OBS source后才能取得
         * m_properties。这里临时创建一个摄像头源用于设备枚举，
         * 枚举完成后立即删除，不改变摄像头的打开状态。
         */
        const Result result = CreateCaptureSource(
            CaptureType::kCT_Camera,
            temporary_source_id);

        if (result != Result::kRet_SUCCESS) {
            return result;
        }

        m_camera_source_id = temporary_source_id;
        property = GetCameraProperty();
    }

    if (!property) {
        if (temporary_source_id != -1) {
            m_camera_source_id = -1;
            DestroyCaptureSource(temporary_source_id);
        }

        LOG_ERROR("Failed to obtain camera source property");
        return Result::kRet_CaptureFailedToCreateProperty;
    }

    const auto capture_devices =
        property->EnumCameraDevices();

    /*
     * 临时属性必须在删除OBS source之前释放。
     */
    if (temporary_source_id != -1) {
        property.reset();

        const Result destroy_result =
            DestroyCaptureSource(temporary_source_id);

        m_camera_source_id = -1;

        if (destroy_result != Result::kRet_SUCCESS) {
            return destroy_result;
        }
    }

    devices.reserve(capture_devices.size());

    for (const auto& capture_device : capture_devices) {
        CameraDeviceInfo device;
        device.name = capture_device.name;
        device.id = capture_device.id;
        devices.emplace_back(std::move(device));
    }

    return Result::kRet_SUCCESS;
}

Result CaptureManager::GetMicrophoneDevices(
    std::vector<MicDeviceInfo>& devices)
{
    devices.clear();

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    std::shared_ptr<CAPTURE::IMicSourceProperty> property;
    int temporary_source_id = -1;

    if (m_microphone_source_id != -1) {
        property = GetMicrophoneProperty();
    } else {
        /*
         * 麦克风属性同样需要绑定真实OBS source，
         * 因此使用临时麦克风源完成设备枚举。
         */
        const Result result = CreateCaptureSource(
            CaptureType::kCT_Mic,
            temporary_source_id);

        if (result != Result::kRet_SUCCESS) {
            return result;
        }

        m_microphone_source_id = temporary_source_id;
        property = GetMicrophoneProperty();
    }

    if (!property) {
        if (temporary_source_id != -1) {
            m_microphone_source_id = -1;
            DestroyCaptureSource(temporary_source_id);
        }

        LOG_ERROR("Failed to obtain microphone source property");
        return Result::kRet_CaptureFailedToCreateProperty;
    }

    const auto capture_devices =
        property->EnumMicDevices();

    if (temporary_source_id != -1) {
        property.reset();

        const Result destroy_result =
            DestroyCaptureSource(temporary_source_id);

        m_microphone_source_id = -1;

        if (destroy_result != Result::kRet_SUCCESS) {
            return destroy_result;
        }
    }

    devices.reserve(capture_devices.size());

    for (const auto& capture_device : capture_devices) {
        MicDeviceInfo device;
        device.name = capture_device.name;
        device.id = capture_device.id;
        devices.emplace_back(std::move(device));
    }

    return Result::kRet_SUCCESS;
}

Result CaptureManager::GetCurrentCameraDeviceId(std::string& camera_device_id) {
    camera_device_id.clear();

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) || !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    /*
     * 摄像头打开时，以当前采集源实际使用的属性为准。
     */
    if (m_camera_source_id != -1) {
        auto property = GetCameraProperty();
        if (!property) {
            return Result::kRet_CaptureFailedToGetDevice;
        }

        const auto current_device = property->GetCurrentDevice();

        if (!current_device) {
            return Result::kRet_CaptureFailedToGetDevice;
        }

        camera_device_id = current_device->id;
        return Result::kRet_SUCCESS;
    }

    /*
     * 摄像头尚未打开时，返回用户之前选择并等待应用的设备。
     */
    if (m_selected_camera_device_id.empty()) {
        return Result::kRet_CaptureFailedToGetDevice;
    }

    camera_device_id = m_selected_camera_device_id;
    return Result::kRet_SUCCESS;
}

Result CaptureManager::GetCurrentMicrophoneDeviceId(std::string& microphone_device_id) {
    microphone_device_id.clear();

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) || !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    if (m_microphone_source_id != -1) {
        auto property = GetMicrophoneProperty();
        if (!property) {
            return Result::kRet_CaptureFailedToGetDevice;
        }

        const auto current_device =
            property->GetCurrentMicDevice();

        if (!current_device) {
            return Result::kRet_CaptureFailedToGetDevice;
        }

        microphone_device_id = current_device->id;
        return Result::kRet_SUCCESS;
    }

    if (m_selected_microphone_device_id.empty()) {
        return Result::kRet_CaptureFailedToGetDevice;
    }

    microphone_device_id = m_selected_microphone_device_id;
    return Result::kRet_SUCCESS;
}

Result CaptureManager::UpdateCameraDevice(
    const std::string& camera_device_id)
{
    if (camera_device_id.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    /*
     * 摄像头尚未打开时只保存设备ID。
     * OpenCamera()创建真实采集源后会将该ID应用到
     * 采集源持有的CameraSourceProperty。
     */
    if (m_camera_source_id == -1) {
        m_selected_camera_device_id = camera_device_id;
        return Result::kRet_SUCCESS;
    }

    auto property = GetCameraProperty();
    if (!property) {
        return Result::kRet_CaptureFailedToCreateProperty;
    }

    if (!property->SetVideoDevice(camera_device_id)) {
        LOG_ERROR(
            "Failed to select camera device: {}",
            camera_device_id);

        return Result::kRet_CaptureFailedToSetProperty;
    }

    m_selected_camera_device_id = camera_device_id;
    return Result::kRet_SUCCESS;
}

Result CaptureManager::ConfigureCameraInput(
    const std::string& video_format_name,
    int width,
    int height)
{
    if (video_format_name.empty() ||
        width <= 0 ||
        height <= 0)
    {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_capture_engine)
    {
        return Result::kRet_CaptureNotInitialized;
    }

    if (m_camera_source_id == -1)
    {
        LOG_ERROR(
            "Failed to configure camera input: camera is not open");

        return Result::kRet_CaptureNoCurrentSource;
    }

    auto property = GetCameraProperty();

    if (!property)
    {
        LOG_ERROR(
            "Failed to configure camera input: property unavailable");

        return Result::kRet_CaptureFailedToCreateProperty;
    }

    /*
     * 视频格式列表依赖当前摄像头设备。
     * OpenCamera已经把之前选择的设备应用到了当前Source。
     */
    const auto formats = property->EnumVideoFormats();

    const auto selected_format = std::find_if(
        formats.begin(),
        formats.end(),
        [&video_format_name](
            const CAPTURE::CameraVideoFormat& format)
        {
            return format.name.find(video_format_name) !=
                   std::string::npos;
        });

    if (selected_format == formats.end())
    {
        LOG_ERROR(
            "Camera does not support requested format: {}",
            video_format_name);

        return Result::kRet_CaptureFailedToSetProperty;
    }

    if (!property->SetVideoFormat(
            selected_format->value))
    {
        LOG_ERROR(
            "Failed to set camera video format: {}",
            selected_format->name);

        return Result::kRet_CaptureFailedToSetProperty;
    }

    /*
     * 分辨率列表依赖当前视频格式，因此必须在设置格式后重新枚举。
     */
    const auto resolutions =
        property->EnumResolutions();

    const auto selected_resolution = std::find_if(
        resolutions.begin(),
        resolutions.end(),
        [width, height](
            const CAPTURE::CameraResolution& resolution)
        {
            return resolution.width ==
                       static_cast<std::uint32_t>(width) &&
                   resolution.height ==
                       static_cast<std::uint32_t>(height);
        });

    if (selected_resolution == resolutions.end())
    {
        LOG_ERROR(
            "Camera format {} does not support resolution {}x{}",
            selected_format->name,
            width,
            height);

        return Result::kRet_CaptureFailedToSetProperty;
    }

    if (!property->SetResolution(
            *selected_resolution))
    {
        LOG_ERROR(
            "Failed to set camera resolution: {}x{}",
            width,
            height);

        return Result::kRet_CaptureFailedToSetProperty;
    }

    const auto current_format =
        property->GetCurrentVideoFormat();

    const auto current_resolution =
        property->GetCurrentResolution();

    if (!current_format ||
        current_format->value != selected_format->value ||
        !current_resolution ||
        current_resolution->width !=
            static_cast<std::uint32_t>(width) ||
        current_resolution->height !=
            static_cast<std::uint32_t>(height))
    {
        LOG_ERROR(
            "Camera input configuration verification failed");

        return Result::kRet_CaptureFailedToSetProperty;
    }

    LOG_INFO(
        "Camera input configured: format={}, size={}x{}",
        current_format->name,
        current_resolution->width,
        current_resolution->height);

    return Result::kRet_SUCCESS;
}


Result CaptureManager::UpdateMicrophoneDevice(
    const std::string& microphone_device_id)
{
    if (microphone_device_id.empty()) {
        return Result::kRet_InvalidParam;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    /*
     * 麦克风尚未打开时只保存ID，
     * OpenMic()创建真实采集源后再应用。
     */
    if (m_microphone_source_id == -1) {
        m_selected_microphone_device_id =
            microphone_device_id;

        return Result::kRet_SUCCESS;
    }

    auto property = GetMicrophoneProperty();
    if (!property) {
        return Result::kRet_CaptureFailedToCreateProperty;
    }

    if (!property->SetMicDevice(microphone_device_id)) {
        LOG_ERROR(
            "Failed to select microphone device: {}",
            microphone_device_id);

        return Result::kRet_CaptureFailedToSetProperty;
    }

    m_selected_microphone_device_id =
        microphone_device_id;

    return Result::kRet_SUCCESS;
}

Result CaptureManager::OpenCamera()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    if (m_camera_source_id != -1) {
        return Result::kRet_SUCCESS;
    }

    /*
     * 先注册回调，确保采集源创建后产生的第一帧不会丢失。
     */
    m_capture_engine->RegisterVideoCallback(
        [this](const std::shared_ptr<I420Frame>& frame) {
            OnEngineVideoFrame(frame);
        });

    int source_id = -1;
    const Result result = CreateCaptureSource(CaptureType::kCT_Camera, source_id);

    if (result != Result::kRet_SUCCESS) {
        m_capture_engine->RegisterVideoCallback({});
        return result;
    }

    m_camera_source_id = source_id;

    /*
     * 如果用户在打开摄像头前选择了设备，
     * 将保存的设备ID应用到当前采集源属性。
     */
    if (!m_selected_camera_device_id.empty()) {
        auto property = GetCameraProperty();

        if (!property ||!property->SetVideoDevice(m_selected_camera_device_id)) {
            LOG_ERROR(
                "Failed to apply selected camera device: {}",
                m_selected_camera_device_id);

            DestroyCaptureSource(m_camera_source_id);
            m_camera_source_id = -1;
            m_capture_engine->RegisterVideoCallback({});

            return Result::kRet_CaptureFailedToSetProperty;
        }
    }

    LOG_INFO("Camera capture opened");
    return Result::kRet_SUCCESS;
}

Result CaptureManager::CloseCamera() {
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) || !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    if (m_camera_source_id == -1) {
        return Result::kRet_SUCCESS;
    }

    m_capture_engine->RegisterVideoCallback({});

    const Result result = DestroyCaptureSource(m_camera_source_id);

    if (result == Result::kRet_SUCCESS) {
        m_camera_source_id = -1;
        LOG_INFO("Camera capture closed");
    }

    return result;
}

Result CaptureManager::OpenMic() {
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) || !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    if (m_microphone_source_id != -1) {
        return Result::kRet_SUCCESS;
    }

    m_capture_engine->RegisterAudioCallback(
        [this](const std::shared_ptr<AudioFrame>& frame) {
            OnEngineAudioFrame(frame);
        });

    int source_id = -1;
    const Result result = CreateCaptureSource(CaptureType::kCT_Mic, source_id);
    if (result != Result::kRet_SUCCESS) {
        m_capture_engine->RegisterAudioCallback({});
        return result;
    }

    m_microphone_source_id = source_id;

    if (!m_selected_microphone_device_id.empty()) {
        auto property = GetMicrophoneProperty();

        if (!property || !property->SetMicDevice(m_selected_microphone_device_id)) {
            LOG_ERROR(
                "Failed to apply selected microphone device: {}",
                m_selected_microphone_device_id);

            DestroyCaptureSource(m_microphone_source_id);
            m_microphone_source_id = -1;
            m_capture_engine->RegisterAudioCallback({});

            return Result::kRet_CaptureFailedToSetProperty;
        }
    }

    LOG_INFO("Microphone capture opened");
    return Result::kRet_SUCCESS;
}

Result CaptureManager::CloseMic()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    if (m_microphone_source_id == -1) {
        return Result::kRet_SUCCESS;
    }

    m_capture_engine->RegisterAudioCallback({});

    const Result result =
        DestroyCaptureSource(m_microphone_source_id);

    if (result == Result::kRet_SUCCESS) {
        m_microphone_source_id = -1;
        LOG_INFO("Microphone capture closed");
    }

    return result;
}

Result CaptureManager::SetCaptureDataCallback(
    const std::shared_ptr<ICaptureDataCallback>& callback)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_capture_callback = callback;
    return Result::kRet_SUCCESS;
}

Result CaptureManager::CreateCaptureSource(CaptureType type, int& source_id) {
    source_id = -1;

    if (!m_capture_engine || !m_initialized.load(std::memory_order_acquire)) {
        return Result::kRet_CaptureNotInitialized;
    }

    CAPTURE::CaptureSourceType capture_type;
    const char* source_type_name = nullptr;

    switch (type) {
    case CaptureType::kCT_Camera:
        capture_type =
            CAPTURE::CaptureSourceType::kCST_Camera;
        source_type_name = "camera";
        break;

    case CaptureType::kCT_Mic:
        capture_type =
            CAPTURE::CaptureSourceType::kCST_Mic;
        source_type_name = "microphone";
        break;

    default:
        return Result::kRet_InvalidParam;
    }

    const int new_source_id =
        m_next_source_id.load(std::memory_order_relaxed);

    const std::string source_name =
        "vce_" +
        std::string(source_type_name) +
        "_" +
        std::to_string(new_source_id);

    auto source = m_capture_engine->CreateSource(capture_type, source_name);
    if (!source) {
        LOG_ERROR(
            "Failed to create capture source: name={}, type={}",
            source_name,
            source_type_name);

        return Result::kRet_CaptureFailedToCreateSource;
    }

    m_source_id_info_map.emplace(
        new_source_id,
        std::make_pair(source_name, type));

    m_next_source_id.fetch_add(
        1,
        std::memory_order_relaxed);

    source_id = new_source_id;
    return Result::kRet_SUCCESS;
}

Result CaptureManager::DestroyCaptureSource(int source_id) {
    const auto source_iterator =
        m_source_id_info_map.find(source_id);

    if (source_iterator ==
        m_source_id_info_map.end()) {
        return Result::kRet_CaptureNoCurrentSource;
    }

    if (!m_capture_engine) {
        return Result::kRet_CaptureNotInitialized;
    }

    const std::string source_name =
        source_iterator->second.first;

    if (!m_capture_engine->RemoveSource(source_name)) {
        LOG_ERROR(
            "Failed to remove capture source: {}",
            source_name);

        return Result::kRet_CaptureNoCurrentSource;
    }

    m_source_id_info_map.erase(source_iterator);
    return Result::kRet_SUCCESS;
}

std::shared_ptr<CAPTURE::ICameraSourceProperty>
CaptureManager::GetCameraProperty() const
{
    if (!m_capture_engine || m_camera_source_id == -1) {
        return nullptr;
    }

    const auto source_iterator =
        m_source_id_info_map.find(m_camera_source_id);

    if (source_iterator == m_source_id_info_map.end()) {
        return nullptr;
    }

    auto source =
        m_capture_engine->GetSource(source_iterator->second.first);

    auto camera_source =
        std::dynamic_pointer_cast<
            CAPTURE::ICameraSource>(source);

    if (!camera_source) {
        return nullptr;
    }

    return camera_source->GetProperty();
}

std::shared_ptr<CAPTURE::IMicSourceProperty> CaptureManager::GetMicrophoneProperty() const
{
    if (!m_capture_engine || m_microphone_source_id == -1) {
        return nullptr;
    }

    const auto source_iterator = m_source_id_info_map.find(m_microphone_source_id);

    if (source_iterator == m_source_id_info_map.end()) {
        return nullptr;
    }

    auto source = m_capture_engine->GetSource(source_iterator->second.first);

    auto microphone_source = std::dynamic_pointer_cast<CAPTURE::IMicSource>(source);
            
    if (!microphone_source) {
        return nullptr;
    }

    return microphone_source->GetProperty();
}

void CaptureManager::OnEngineVideoFrame(const std::shared_ptr<I420Frame>& frame) {
    if (!frame || !frame->IsValid()) {
        return;
    }

    std::shared_ptr<ICaptureDataCallback> callback;

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback = m_capture_callback.lock();
    }

    if (!callback) {
        return;
    }

    /*
     * 不在持有m_callback_mutex时调用外部对象，
     * 避免回调中再次调用Manager引发死锁。
     */
    try {
        callback->OnCaptureVideoFrame(frame);
    } catch (const std::exception& exception) {
        LOG_ERROR("Capture video callback failed: {}", exception.what());
            
    } catch (...) {
        LOG_ERROR("Capture video callback failed: unknown exception");
            
    }
}

void CaptureManager::OnEngineAudioFrame(const std::shared_ptr<AudioFrame>& frame)
{
    if (!frame || !frame->IsValid()) {
        return;
    }

    std::shared_ptr<ICaptureDataCallback> callback;

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback = m_capture_callback.lock();
    }

    if (!callback) {
        return;
    }

    try {
        callback->OnCaptureAudioFrame(frame);
    } catch (const std::exception& exception) {
        LOG_ERROR("Capture audio callback failed: {}", exception.what());
            
    } catch (...) {
        LOG_ERROR("Capture audio callback failed: unknown exception");
            
    }
}


} // namespace VCE