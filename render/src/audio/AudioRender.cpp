#include "AudioRender.h"

#include "utils/logManager.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>

namespace RENDER
{
AudioRender::AudioRender() = default;

AudioRender::~AudioRender() {
    Uninitialize();
    m_audio_source = nullptr;
}

void AudioRender::SetAudioSource(std::shared_ptr<IAudioSource> source) noexcept {
    std::atomic_store_explicit(&m_audio_source, std::move(source), std::memory_order_release);
}
    
bool AudioRender::Initialize(int sample_rate, int channels) {
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);

    if (sample_rate <= 0 || channels <= 0 || channels > 8) {
        LOG_ERROR("Invalid AudioRender parameters: "
            "sample_rate={}, channels={}",
            sample_rate, channels);
            
        return false;
    }

    if (m_initialized.load(std::memory_order_acquire)) {
        if (m_sample_rate.load(std::memory_order_acquire) == sample_rate &&
            m_channels.load(std::memory_order_acquire) == channels) {
            return true;
        }

        // 参数发生变化时，先关闭原工作线程和设备
        m_initialized.store(false, std::memory_order_release);
        
        if (m_worker_running.load(std::memory_order_acquire)) {
            SendCommandAndWait(AudioCommand::kShutdown);
        }
        
        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }

        m_worker_running.store(false, std::memory_order_release);
            
    } else if (m_worker_thread.joinable()) {
        if (m_worker_running.load(std::memory_order_acquire)) {
            SendCommandAndWait(AudioCommand::kShutdown);
        }
        m_worker_thread.join();
        m_worker_running.store(false, std::memory_order_release);
    }

    m_sample_rate.store(sample_rate, std::memory_order_release);

    m_channels.store(channels, std::memory_order_release);

    m_worker_running.store(true, std::memory_order_release);
        
    try
    {
        m_worker_thread = std::thread(&AudioRender::WorkerThreadFunc, this);
    }
    catch(const std::exception& e)
    {
        m_worker_running.store(false, std::memory_order_release);
        m_sample_rate.store(0);
        m_channels.store(0);
        LOG_ERROR("Failed to create AudioRender worker thread: {}", e.what());
        return false;
    }

    std::string preferred_device;
    {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        preferred_device = m_current_device_id;
    }

    if (!SendCommandAndWait(AudioCommand::kInitialize, preferred_device)) {
        LOG_ERROR("Failed to initialize audio playback device");

        if (m_worker_running.load(std::memory_order_acquire)) {
            SendCommandAndWait(AudioCommand::kShutdown);
        }

        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }

        m_worker_running.store(false, std::memory_order_release);

        m_sample_rate.store(0);
        m_channels.store(0);

        return false;
    }

    m_initialized.store(true, std::memory_order_release);
        
    LOG_INFO("AudioRender initialized: sample_rate={}, channels={}",
        sample_rate, channels);

    return true;
}
    
void AudioRender::Uninitialize() {
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
    m_initialized.store(false, std::memory_order_release);

    if (m_worker_running.load(std::memory_order_acquire)) {
        SendCommandAndWait(AudioCommand::kShutdown);
    }

    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }

    m_worker_running.store(false, std::memory_order_release);
        
    m_sample_rate.store(0, std::memory_order_release);

    m_channels.store(0, std::memory_order_release);

    LOG_INFO("AudioRender uninitialized");
}

bool AudioRender::EnumSpeakers(std::vector<AudioSpeaker>& devices) {
    devices.clear();
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);

    if (!m_initialized.load(std::memory_order_acquire) ||
        !m_worker_running.load(std::memory_order_acquire)) {
        return false;
    }

    return SendCommandAndWait(AudioCommand::kEnumerateSpeakers,{}, &devices);
}

bool AudioRender::SetSpeaker(const std::string& device_id) {
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);

    std::string current_device;

    {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        current_device = m_current_device_id;
    }

    if (device_id == current_device) {
        return true;
    }

    /*
     * AudioRender尚未初始化时，只保存首选设备。
     */
    if (!m_worker_running.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        m_current_device_id = device_id;
        return true;
    }

    return SendCommandAndWait(AudioCommand::kSwitchDevice, device_id);
}
    
std::string AudioRender::GetCurrentSpeakerId() const {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    return m_current_device_id;
}


bool AudioRender::SendCommandAndWait(
    AudioCommand command, 
    const std::string& device_id,
    std::vector<AudioSpeaker>* speakers) {

    /*
     * 保证同一时间只有一个调用者提交命令并等待结果。
     */
    std::lock_guard<std::mutex> submit_lock(m_command_submit_mutex);
        
    std::unique_lock<std::mutex> lock(m_worker_state.mutex);
    
    if (!m_worker_running.load(std::memory_order_acquire)) {
        return false;
    }

    m_worker_state.command = command;
    m_worker_state.target_device_id = device_id;

    m_worker_state.command_pending = true;
    m_worker_state.command_completed = false;
    m_worker_state.command_success = false;
    m_worker_state.speaker_result.clear();

    m_worker_state.command_cv.notify_one();

    m_worker_state.completion_cv.wait(lock, [this]() {
        return m_worker_state.command_completed ||
                !m_worker_running.load(std::memory_order_acquire);
    });

    if (!m_worker_state.command_completed) {
        return false;
    }

    if (speakers) {
        *speakers = m_worker_state.speaker_result;
    }

    return m_worker_state.command_success;
}
    
void AudioRender::WorkerThreadFunc(AudioRender* self) noexcept {
    if (!self) {
        return;
    }

    while (true) {
        AudioCommand command{AudioCommand::kNone};
        std::string target_device_id;

        {
            std::unique_lock<std::mutex> lock(self->m_worker_state.mutex);
            self->m_worker_state.command_cv.wait(lock, [self]() {
                return self->m_worker_state.command_pending ||
                        !self->m_worker_running.load(std::memory_order_acquire);
            });

            if (!self->m_worker_running.load(std::memory_order_acquire) &&
                !self->m_worker_state.command_pending) {
                return;
            }

            command = self->m_worker_state.command;

            target_device_id = self->m_worker_state.target_device_id;

            self->m_worker_state.command_pending = false;
        }

        bool command_success = false;
        bool should_exit = false;
        std::vector<AudioSpeaker> speaker_result;

        try {
            switch (command) {
            case AudioCommand::kInitialize: {
                if (!self->m_worker_state.context_initialized) {

                    // 
                    const ma_result result = ma_context_init(nullptr, 0, nullptr, &self->m_worker_state.context);
                            
                    if (result != MA_SUCCESS) {
                        LOG_ERROR("Failed to initialize miniaudio context: {}",
                            static_cast<int>(result));
                        break;
                    }

                    self->m_worker_state.context_initialized = true;
                }

                command_success = self->InitDeviceInternal(target_device_id);
                /*
                 * 保存的首选设备可能已经不存在。
                 * 初始化阶段允许自动回退到默认设备。
                 */
                if (!command_success && !target_device_id.empty()) {
                    LOG_WARN("Preferred speaker was unavailable,falling back to default device");

                    command_success = self->InitDeviceInternal({});
                }    

                if (!command_success) {
                    if (self->m_worker_state.device_initialized) {
                        ma_device_uninit(&self->m_worker_state.device);
                        self->m_worker_state.device_initialized =false;
                    }

                    if (self->m_worker_state.context_initialized) {
                        ma_context_uninit(&self->m_worker_state.context);
                        self->m_worker_state.context_initialized = false;
                    }
                }

                break;
            }

            case AudioCommand::kStart: {
                if (!self->m_worker_state.device_initialized) {
                    break;
                }

                const ma_result result = ma_device_start(&self->m_worker_state.device);

                command_success = result == MA_SUCCESS;

                if (!command_success) {
                    LOG_ERROR("Failed to start audio device: {}", static_cast<int>(result));
                }

                break;
            }

            case AudioCommand::kStop: {
                if (!self->m_worker_state.device_initialized) {
                    command_success = true;
                    break;
                }

                const ma_result result = ma_device_stop(&self->m_worker_state.device);

                command_success = result == MA_SUCCESS;

                if (!command_success) {
                    LOG_ERROR(
                        "Failed to stop audio device: {}",
                        static_cast<int>(result));
                }

                break;
            }

            case AudioCommand::kSwitchDevice: {
                if (!self->m_worker_state.context_initialized) {
                    break;
                }

                std::string previous_device;

                {
                    std::lock_guard<std::mutex> lock(self->m_device_mutex);
                    previous_device = self->m_current_device_id;
                }

                if (target_device_id == previous_device) {
                    break;
                }

                if (self->m_worker_state.device_initialized) {
                    ma_device_uninit(&self->m_worker_state.device);

                    self->m_worker_state.device_initialized = false;
                }

                command_success = self->InitDeviceInternal(target_device_id);
                 /*
                 * 目标设备打开失败时，尝试恢复原播放设备。
                 */
                if (!command_success) {
                    LOG_ERROR("Failed to switch speaker to: {}",
                        target_device_id);

                    bool restored = false;

                    if (previous_device != target_device_id) {
                        restored = self->InitDeviceInternal(previous_device);
                    }

                    if (!restored) {
                        restored = self->InitDeviceInternal({});
                    }

                    self->m_initialized.store(restored, std::memory_order_release);
                }
                break;
            }

            case AudioCommand::kEnumerateSpeakers: {
                command_success = self->EnumSpeakersInternal(speaker_result);
                break;
            }

            case AudioCommand::kShutdown: {
                if (self->m_worker_state.device_initialized) {
                    ma_device_uninit(&self->m_worker_state.device);
                    self->m_worker_state.device_initialized = false;
                }

                if (self->m_worker_state.context_initialized) {
                    ma_context_uninit(&self->m_worker_state.context);
                    self->m_worker_state.context_initialized = false;
                }

                command_success = true;
                should_exit = true;
                break;
            }

            case AudioCommand::kNone:
            default:
                LOG_ERROR("AudioRender received an unknown command");
                break;
            }
        } catch(const std::exception& e) {
            LOG_ERROR("AudioRender worker command failed: {}", e.what());
            command_success = false;
        } catch (...) {
            LOG_ERROR("AudioRender worker command failed "
                "with an unknown exception");

            command_success = false;
        }

        {
            std::lock_guard<std::mutex> lock(self->m_worker_state.mutex);

            self->m_worker_state.command = AudioCommand::kNone;

            self->m_worker_state.target_device_id.clear();

            self->m_worker_state.speaker_result = std::move(speaker_result);

            self->m_worker_state.command_success = command_success;

            self->m_worker_state.command_completed = true;
        }

        if (should_exit) {
            self->m_worker_running.store(false, std::memory_order_release);
        }

        self->m_worker_state.completion_cv.notify_all();

        if (should_exit) {
            return;
        }
    }
}


void AudioRender::DataCallback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) noexcept {
    (void)input;

    if (!device || !output) {
        return;
    }

    auto* self = static_cast<AudioRender*>(device->pUserData);

    if (!self) {
        return;
    }

    self->ProcessAudioCallback(output, frame_count);
}
    
    
void AudioRender::ProcessAudioCallback(void* output, ma_uint32 frame_count) noexcept {
    if (!output || frame_count == 0) {
        return;
    }

    const int channels = m_channels.load(std::memory_order_acquire);

    if (channels <= 0) {
        return;
    }

    auto* output_samples = static_cast<float*>(output);

    std::uint32_t written_frames = 0;

    if (m_initialized.load(std::memory_order_acquire)) {
        const auto source = std::atomic_load_explicit(&m_audio_source, std::memory_order_acquire);
        
        if (source) {
            written_frames = source->PopAudio(output_samples, frame_count);

            if (written_frames > frame_count) {
                written_frames = frame_count;
            }
        }
    }

    /*
     * 没有数据源或读取不足时，使用静音补齐。
     */
    if (written_frames < frame_count) {
        const std::size_t written_samples =
            static_cast<std::size_t>(written_frames) *
            static_cast<std::size_t>(channels);

        const std::size_t missing_samples =
            static_cast<std::size_t>(frame_count - written_frames) 
                * static_cast<std::size_t>(channels);
            
        std::memset(output_samples + written_samples, 0, missing_samples * sizeof(float));
    }
}

bool AudioRender::InitDeviceInternal(const std::string& device_id) {
    if (!m_worker_state.context_initialized) {
        return false;
    }

    const int sample_rate = m_sample_rate.load(std::memory_order_acquire);
        
    const int channels = m_channels.load(std::memory_order_acquire);

    if (sample_rate <= 0 || channels <= 0) {
        return false;
    }

    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;

    // 
    const ma_result enum_result =
        ma_context_get_devices(&m_worker_state.context, &playback_infos,
            &playback_count, nullptr, nullptr);
            
    if (enum_result != MA_SUCCESS) {
        LOG_ERROR("Failed to enumerate playback devices: {}", static_cast<int>(enum_result));
        return false;
    }   
            
    if (!playback_infos || playback_count == 0) {
        LOG_ERROR("No audio playback device was found");
        return false;
    }
            
    const ma_device_id* selected_device_id = nullptr;
    std::string selected_device_name;

    if (device_id.empty()) {
        /*
         * pDeviceID保持nullptr，让miniaudio使用系统默认设备。
         * 同时记录默认设备名称供上层查询。
         */
        for (ma_uint32 index = 0; index < playback_count; ++index) {
            if (playback_infos[index].isDefault == MA_TRUE) {
                selected_device_name = playback_infos[index].name;
                break;
            }
        }
        if (selected_device_name.empty()) {
            selected_device_name = playback_infos[0].name;
        }

    } else {
        for (ma_uint32 index = 0; index < playback_count; ++index) {
            if (device_id == playback_infos[index].name) {
                selected_device_id = &playback_infos[index].id;
                selected_device_name = playback_infos[index].name;
                break;
            }
        }

        if (!selected_device_id) {
            LOG_ERROR("Playback device was not found: {}", device_id);
            return false;
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);

    config.playback.pDeviceID = selected_device_id;

    config.playback.format = ma_format_f32;

    config.playback.channels = static_cast<ma_uint32>(channels);

    config.sampleRate = static_cast<ma_uint32>(sample_rate);

    config.periodSizeInFrames = std::max(1, sample_rate / 100);
        
    config.dataCallback = &AudioRender::DataCallback;
    
    config.pUserData = this;

    const ma_result init_result =
        ma_device_init(&m_worker_state.context, &config, &m_worker_state.device);
            
    if (init_result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize playback device {}: {}",
            selected_device_name,
            static_cast<int>(init_result));
            
        return false;
    }

    m_worker_state.device_initialized = true;
            
    const ma_result start_result = ma_device_start(&m_worker_state.device);
    if (start_result != MA_SUCCESS) {
        LOG_ERROR("Failed to start playback device {}: {}",
            selected_device_name,static_cast<int>(start_result));

        ma_device_uninit(&m_worker_state.device);

        m_worker_state.device_initialized = false;
        return false;
    }
            
    {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        m_current_device_id = selected_device_name;
    }  

    LOG_INFO(
        "Audio playback started: device={}, "
        "sample_rate={}, channels={}",
        selected_device_name,
        sample_rate,
        channels);

    return true;
}
    
bool AudioRender::EnumSpeakersInternal(std::vector<AudioSpeaker>& speakers) {
    speakers.clear();

    if (!m_worker_state.context_initialized) {
        return false;
    }

    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;

    // 
    const ma_result result = ma_context_get_devices(&m_worker_state.context,
            &playback_infos, &playback_count, nullptr, nullptr);
        
     if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to enumerate playback devices: {}",
            static_cast<int>(result));
            
        return false;
    }

    if (!playback_infos || playback_count == 0) {
        return false;
    }     
    speakers.reserve(playback_count);
    
    for (ma_uint32 index = 0; index < playback_count; ++index) {

        const char* name = playback_infos[index].name;
        if (!name || *name == '\0') {
            continue;
        }

        /*
         * miniaudio的ma_device_id是平台相关的内部结构，
         * 当前使用设备名称作为模块对外设备ID。
         */
        speakers.emplace_back(name, name, playback_infos[index].isDefault == MA_TRUE);
    }

    return !speakers.empty();
}


} // namespace RENDER
