cmake_minimum_required(VERSION 3.16)

project(meeting_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt客户端默认关闭，避免未安装Qt时影响原工程配置
option(BUILD_QT_CLIENT "Build the Qt meeting client" OFF)

# 已完成的独立测试默认不再参与构建
option(BUILD_VCE_TESTS "Build capture, render, codec and media tests" OFF)

# ============================================================
# 公共依赖
# ============================================================

find_package(PkgConfig REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ============================================================
# OBS采集依赖
# ============================================================

pkg_check_modules(
    LIBOBS
    REQUIRED
    IMPORTED_TARGET
    libobs
)

# ============================================================
# OpenGL视频渲染依赖
# ============================================================

find_package(OpenGL REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

# ============================================================
# 音视频传输依赖
# ============================================================

# WebRTC、ICE、DTLS、SRTP和RTP传输
find_package(
    LibDataChannel
    CONFIG
    REQUIRED
)

# WHIP/WHEP HTTP信令
find_package(
    CURL
    REQUIRED
)

# libdatachannel使用的TLS/DTLS后端
find_package(
    OpenSSL
    REQUIRED
)

# Opus音频编解码
pkg_check_modules(
    OPUS
    REQUIRED
    IMPORTED_TARGET
    opus
)

# OpenH264视频编解码
pkg_check_modules(
    OPENH264
    REQUIRED
    IMPORTED_TARGET
    openh264
)

# 音频重采样
pkg_check_modules(
    LIBSAMPLERATE
    REQUIRED
    IMPORTED_TARGET
    samplerate
)

find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# ============================================================
# miniaudio
#
# vcpkg的miniaudio端口只安装头文件，
# 没有提供miniaudioConfig.cmake。
# ============================================================

find_path(
    MINIAUDIO_INCLUDE_DIR
    NAMES miniaudio.h
)

if(NOT MINIAUDIO_INCLUDE_DIR)
    message(FATAL_ERROR
        "miniaudio.h not found. "
        "Please configure CMake with the vcpkg toolchain file."
    )
endif()

message(
    STATUS
    "Found miniaudio: ${MINIAUDIO_INCLUDE_DIR}"
)

add_library(
    miniaudio_target
    INTERFACE
)

target_include_directories(
    miniaudio_target
    INTERFACE
        ${MINIAUDIO_INCLUDE_DIR}
)

add_library(
    miniaudio::miniaudio
    ALIAS
    miniaudio_target
)

# ============================================================
# 独立模块测试
#
# Qt客户端已经在VideoConferenceClient中独立列出运行时源码，
# 以下源码变量和测试目标只在需要重新测试底层模块时启用。
# ============================================================
if(BUILD_VCE_TESTS)

# ============================================================
# 采集模块源码
# ============================================================

set(CAPTURE_SOURCES
    capture/src/CaptureEngine/CaptureEngine.cpp

    capture/src/property/CameraSourceProperty.cpp
    capture/src/property/MicSourceProperty.cpp
)

# ============================================================
# 渲染模块源码
# ============================================================

set(RENDER_SOURCES
    render/src/core/RenderEngine.cpp

    render/src/audio/MiniaudioImpl.cpp
    render/src/audio/AudioMixer.cpp
    render/src/audio/AudioRender.cpp
    render/src/audio/TimestampedAudioBuffer.cpp
    render/src/audio/AudioPlaybackClock.cpp
    render/src/audio/AdaptiveJitterController.cpp

    render/src/video/GLFrameWork/VideoShader.cpp
    render/src/video/GLFrameWork/VideoMesh.cpp
    render/src/video/GLFrameWork/YUVTexture.cpp

    render/src/video/VideoRender.cpp
    render/src/video/UserContext.cpp

    render/src/core/AVSyncController.cpp
)

# ============================================================
# 传输模块编解码源码
# ============================================================

set(TRANSPORT_CODEC_SOURCES
    transport/src/codec/audio_codec/AudioCodecFactory.cpp
    transport/src/codec/audio_codec/OpusCodec.cpp

    transport/src/codec/video_codec/VideoCodecFactory.cpp
    transport/src/codec/video_codec/H264Codec.cpp
)

# ============================================================
# 完整传输模块源码
# ============================================================

set(TRANSPORT_SOURCES
    ${TRANSPORT_CODEC_SOURCES}

    transport/src/transport/TransportEngine.cpp
    transport/src/transport/RtcTransportBase.cpp
    transport/src/transport/RtcPushTransport.cpp
    transport/src/transport/RtcPullTransport.cpp
    transport/src/transport/WhipWhepClient.cpp
    transport/src/transport/VideoRtpReorderBuffer.cpp
    transport/src/transport/RtpNtpTimeMapper.cpp
)

# ============================================================
# VideoConferenceEngine源码
# ============================================================

set(VCE_MEDIA_MANAGER_SOURCES
    VideoConferenceEngine/private/core/capture_wrapper/CaptureManager.cpp
    VideoConferenceEngine/private/core/render_wrapper/RenderManager.cpp
    VideoConferenceEngine/private/core/transport_wrapper/TransportManager.cpp
)

# ============================================================
# 采集模块测试程序
# ============================================================

add_executable(
    capture_test

    test/capture_test.cpp
    ${CAPTURE_SOURCES}
)

target_include_directories(
    capture_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}

        ${CMAKE_CURRENT_SOURCE_DIR}/capture/include
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/src
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/src/CaptureEngine
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/src/property
)

target_link_libraries(
    capture_test
    PRIVATE
        PkgConfig::LIBOBS

        spdlog::spdlog
        fmt::fmt
        Threads::Threads
)

target_compile_definitions(
    capture_test
    PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

# ============================================================
# 渲染模块测试程序
# ============================================================

add_executable(
    render_test

    test/render_test.cpp
    ${RENDER_SOURCES}
)

target_include_directories(
    render_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}

        # RenderDefine.h引用了采集模块的公共帧类型
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/include

        ${CMAKE_CURRENT_SOURCE_DIR}/render/include
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/audio
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/video
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/video/GLFrameWork
)

target_link_libraries(
    render_test
    PRIVATE
        spdlog::spdlog
        fmt::fmt
        Threads::Threads

        miniaudio::miniaudio

        OpenGL::GL
        glad::glad
        glfw

        # Linux下miniaudio使用的系统库
        ${CMAKE_DL_LIBS}
        m
)

target_compile_definitions(
    render_test
    PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

# ============================================================
# 音视频编解码测试程序
# ============================================================

add_executable(
    codec_test

    test/codec_test.cpp
    ${TRANSPORT_CODEC_SOURCES}
)

target_include_directories(
    codec_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}

        # 公共I420Frame和AudioFrame
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/include

        ${CMAKE_CURRENT_SOURCE_DIR}/transport/include
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec/audio_codec
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec/video_codec
)

target_link_libraries(
    codec_test
    PRIVATE
        # Opus音频编解码
        PkgConfig::OPUS

        # OpenH264视频编解码
        PkgConfig::OPENH264

        # 非48kHz音频重采样
        PkgConfig::LIBSAMPLERATE

        spdlog::spdlog
        fmt::fmt
        Threads::Threads
)

target_compile_definitions(
    codec_test
    PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

# ============================================================
# RTC传输测试程序
# ============================================================

add_executable(
    rtc_transport_test

    test/rtc_transport_test.cpp
    ${TRANSPORT_SOURCES}
)

target_include_directories(
    rtc_transport_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}

        # 公共音视频帧类型
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/include

        ${CMAKE_CURRENT_SOURCE_DIR}/transport/include
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec/audio_codec
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec/video_codec
)

target_link_libraries(
    rtc_transport_test
    PRIVATE
        # WebRTC、ICE、DTLS、SRTP和RTP
        LibDataChannel::LibDataChannel

        # WHIP/WHEP HTTP请求
        CURL::libcurl

        # TLS依赖
        OpenSSL::SSL
        OpenSSL::Crypto

        # 编解码及重采样
        PkgConfig::OPUS
        PkgConfig::OPENH264
        PkgConfig::LIBSAMPLERATE

        spdlog::spdlog
        fmt::fmt
        Threads::Threads

        ${CMAKE_DL_LIBS}
        m
)

target_compile_definitions(
    rtc_transport_test
    PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

# ============================================================
# VideoConferenceEngine完整集成测试
# ============================================================

add_executable(
    vce_media_test

    test/vce_media_test.cpp

    ${VCE_MEDIA_MANAGER_SOURCES}
    ${CAPTURE_SOURCES}
    ${RENDER_SOURCES}
    ${TRANSPORT_SOURCES}
)

target_include_directories(
    vce_media_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}

        # 采集模块
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/include
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/src
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/src/CaptureEngine
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/src/property

        # 渲染模块
        ${CMAKE_CURRENT_SOURCE_DIR}/render/include
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/audio
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/video
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/video/GLFrameWork

        # 传输模块
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/include
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/transport
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec/audio_codec
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/src/codec/video_codec

        # VideoConferenceEngine公开接口
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoConferenceEngine/public/include

        # VideoConferenceEngine核心
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoConferenceEngine/private/core

        # Manager包装层
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoConferenceEngine/private/core/capture_wrapper
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoConferenceEngine/private/core/render_wrapper
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoConferenceEngine/private/core/transport_wrapper

        # Service模块
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoConferenceEngine/private/core/service
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoConferenceEngine/private/core/service/rpc


        # protobuf及gRPC生成头文件
        ${SERVICE_RPC_DIR}
)

target_link_libraries(
    vce_media_test
    PRIVATE
        # OBS采集
        PkgConfig::LIBOBS

        # OpenGL渲染
        OpenGL::GL
        glad::glad
        glfw
        miniaudio::miniaudio

        # WebRTC传输
        LibDataChannel::LibDataChannel
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto

        # 音视频编解码
        PkgConfig::OPUS
        PkgConfig::OPENH264
        PkgConfig::LIBSAMPLERATE

        # 业务服务
        protobuf::libprotobuf
        gRPC::grpc++

        # 公共依赖
        spdlog::spdlog
        fmt::fmt
        Threads::Threads

        # Linux系统库
        ${CMAKE_DL_LIBS}
        m
)

target_compile_definitions(
    vce_media_test
    PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

# 同步模块测试

add_executable(
    audio_mixer_test

    test/audio_mixer_test.cpp

    render/src/audio/AudioMixer.cpp
    render/src/audio/TimestampedAudioBuffer.cpp
    render/src/audio/AudioPlaybackClock.cpp
    render/src/audio/AdaptiveJitterController.cpp
)

target_include_directories(
    audio_mixer_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/include
        ${CMAKE_CURRENT_SOURCE_DIR}/render/include
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src
        ${CMAKE_CURRENT_SOURCE_DIR}/render/src/audio
)

target_link_libraries(
    audio_mixer_test
    PRIVATE
        spdlog::spdlog
        Threads::Threads
)

target_compile_definitions(
    audio_mixer_test
    PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

endif() # BUILD_VCE_TESTS

# ============================================================
# Qt会议客户端
#
# Qt代码和VideoConferenceEngine运行时目标由子目录独立管理。
# 默认关闭，避免影响原有命令行测试和未安装Qt的环境。
# ============================================================

if(BUILD_QT_CLIENT)
    add_subdirectory(VideoConferenceClient)
endif()

# ============================================================
# 本地gRPC测试服务端
#
# 只用于Qt客户端联调，不属于生产服务端。
# ============================================================

if(BUILD_VCE_TEST_SERVER)
    add_subdirectory(VideoConferenceServer)
endif()