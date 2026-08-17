cmake_minimum_required(VERSION 3.16)

# 本目录通过主工程add_subdirectory()进入。
# 底层第三方依赖由主目录CMakeLists.txt统一查找。
get_filename_component(
    VCE_PROJECT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    ABSOLUTE
)

find_package(
    Qt6 6.5
    REQUIRED
    COMPONENTS
        Core
        Gui
        Widgets
        OpenGL
        OpenGLWidgets
)

# ============================================================
# VideoConferenceEngine运行库
# ============================================================

set(VCE_ENGINE_SOURCES
    # 对外接口
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/public/src/VceEngine.cpp

    # 引擎核心
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/VceEngineImpl.cpp

    # Manager包装层
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/capture_wrapper/CaptureManager.cpp
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/render_wrapper/RenderManager.cpp
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/transport_wrapper/TransportManager.cpp

    # 业务服务
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/UserService.cpp
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/MeetingService.cpp
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/ServiceManager.cpp

    # protobuf和gRPC生成代码
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/user_service.pb.cc
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/user_service.grpc.pb.cc
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/meeting_service.pb.cc
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/meeting_service.grpc.pb.cc

    # Capture模块
    ${VCE_PROJECT_ROOT}/capture/src/CaptureEngine/CaptureEngine.cpp
    ${VCE_PROJECT_ROOT}/capture/src/property/CameraSourceProperty.cpp
    ${VCE_PROJECT_ROOT}/capture/src/property/MicSourceProperty.cpp

    # Render模块
    ${VCE_PROJECT_ROOT}/render/src/core/RenderEngine.cpp
    ${VCE_PROJECT_ROOT}/render/src/core/AVSyncController.cpp

    ${VCE_PROJECT_ROOT}/render/src/audio/MiniaudioImpl.cpp
    ${VCE_PROJECT_ROOT}/render/src/audio/AudioMixer.cpp
    ${VCE_PROJECT_ROOT}/render/src/audio/AudioRender.cpp
    ${VCE_PROJECT_ROOT}/render/src/audio/AudioPlaybackClock.cpp
    ${VCE_PROJECT_ROOT}/render/src/audio/TimestampedAudioBuffer.cpp
    ${VCE_PROJECT_ROOT}/render/src/audio/AdaptiveJitterController.cpp

    ${VCE_PROJECT_ROOT}/render/src/video/VideoRender.cpp
    ${VCE_PROJECT_ROOT}/render/src/video/UserContext.cpp
    ${VCE_PROJECT_ROOT}/render/src/video/GLFrameWork/VideoShader.cpp
    ${VCE_PROJECT_ROOT}/render/src/video/GLFrameWork/VideoMesh.cpp
    ${VCE_PROJECT_ROOT}/render/src/video/GLFrameWork/YUVTexture.cpp

    # Transport模块
    ${VCE_PROJECT_ROOT}/transport/src/codec/audio_codec/AudioCodecFactory.cpp
    ${VCE_PROJECT_ROOT}/transport/src/codec/audio_codec/OpusCodec.cpp
    ${VCE_PROJECT_ROOT}/transport/src/codec/video_codec/VideoCodecFactory.cpp
    ${VCE_PROJECT_ROOT}/transport/src/codec/video_codec/H264Codec.cpp

    ${VCE_PROJECT_ROOT}/transport/src/transport/TransportEngine.cpp
    ${VCE_PROJECT_ROOT}/transport/src/transport/RtcTransportBase.cpp
    ${VCE_PROJECT_ROOT}/transport/src/transport/RtcPushTransport.cpp
    ${VCE_PROJECT_ROOT}/transport/src/transport/RtcPullTransport.cpp
    ${VCE_PROJECT_ROOT}/transport/src/transport/WhipWhepClient.cpp
    ${VCE_PROJECT_ROOT}/transport/src/transport/VideoRtpReorderBuffer.cpp
    ${VCE_PROJECT_ROOT}/transport/src/transport/RtpNtpTimeMapper.cpp
)

add_library(
    vce_engine_runtime
    STATIC
    ${VCE_ENGINE_SOURCES}
)

target_compile_features(
    vce_engine_runtime
    PUBLIC
        cxx_std_20
)

target_include_directories(
    vce_engine_runtime

    PUBLIC
        # VceEngine.h通过VceTypes.h继续引用ICaptureEngine.h
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/public/include
        ${VCE_PROJECT_ROOT}/capture/include

    PRIVATE
        ${VCE_PROJECT_ROOT}

        # Capture模块
        ${VCE_PROJECT_ROOT}/capture/src
        ${VCE_PROJECT_ROOT}/capture/src/CaptureEngine
        ${VCE_PROJECT_ROOT}/capture/src/property

        # Render模块
        ${VCE_PROJECT_ROOT}/render/include
        ${VCE_PROJECT_ROOT}/render/src
        ${VCE_PROJECT_ROOT}/render/src/core
        ${VCE_PROJECT_ROOT}/render/src/audio
        ${VCE_PROJECT_ROOT}/render/src/video
        ${VCE_PROJECT_ROOT}/render/src/video/GLFrameWork

        # Transport模块
        ${VCE_PROJECT_ROOT}/transport/include
        ${VCE_PROJECT_ROOT}/transport/src
        ${VCE_PROJECT_ROOT}/transport/src/codec
        ${VCE_PROJECT_ROOT}/transport/src/codec/audio_codec
        ${VCE_PROJECT_ROOT}/transport/src/codec/video_codec
        ${VCE_PROJECT_ROOT}/transport/src/transport

        # VideoConferenceEngine内部实现
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/capture_wrapper
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/render_wrapper
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/transport_wrapper
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc
)

target_link_libraries(
    vce_engine_runtime

    PUBLIC
        Threads::Threads

    PRIVATE
        # OBS采集
        PkgConfig::LIBOBS

        # OpenGL渲染
        OpenGL::GL
        glad::glad
        glfw
        miniaudio::miniaudio

        # RTC传输
        LibDataChannel::LibDataChannel
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto

        # 编解码与音频重采样
        PkgConfig::OPUS
        PkgConfig::OPENH264
        PkgConfig::LIBSAMPLERATE

        # 业务服务
        protobuf::libprotobuf
        gRPC::grpc++

        # 公共依赖
        spdlog::spdlog
        fmt::fmt
)

target_compile_definitions(
    vce_engine_runtime
    PRIVATE
        $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

if(UNIX AND NOT APPLE)
    target_link_libraries(
        vce_engine_runtime
        PRIVATE
            ${CMAKE_DL_LIBS}
            m
    )
endif()

# ============================================================
# Qt客户端
# ============================================================

set(VCE_QT_CLIENT_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h

    src/MeetingEventBridge.cpp
    src/MeetingEventBridge.h

    src/widgets/LoginPage.cpp
    src/widgets/LoginPage.h

    src/widgets/MeetingLobbyPage.cpp
    src/widgets/MeetingLobbyPage.h

    src/widgets/ConferencePage.cpp
    src/widgets/ConferencePage.h

    src/widgets/VideoWidget.cpp
    src/widgets/VideoWidget.h
)

add_executable(
    vce_meeting_client
    ${VCE_QT_CLIENT_SOURCES}
)

set_target_properties(
    vce_meeting_client
    PROPERTIES
        AUTOMOC ON
        AUTOUIC OFF
        AUTORCC OFF

        # 输出到主build目录，使main.cpp默认能找到../config/client.ini
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_features(
    vce_meeting_client
    PRIVATE
        cxx_std_20
)

target_include_directories(
    vce_meeting_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/widgets
)

target_link_libraries(
    vce_meeting_client
    PRIVATE
        vce_engine_runtime

        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_compile_definitions(
    vce_meeting_client
    PRIVATE
        $<$<CONFIG:Debug>:QT_MESSAGELOGCONTEXT>
)