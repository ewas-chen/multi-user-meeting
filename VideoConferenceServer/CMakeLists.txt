cmake_minimum_required(VERSION 3.16)

# 本目录通过主工程add_subdirectory()进入
get_filename_component(
    VCE_PROJECT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    ABSOLUTE
)

set(VCE_TEST_SERVER_SOURCES
    src/main.cpp

    src/InMemoryMeetingStore.cpp
    src/InMemoryMeetingStore.h

    src/UserServiceImpl.cpp
    src/UserServiceImpl.h

    src/MeetingServiceImpl.cpp
    src/MeetingServiceImpl.h

    # 直接复用客户端当前使用的proto生成代码，
    # 保证测试服务端和客户端协议完全一致。
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/user_service.pb.cc
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/user_service.grpc.pb.cc
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/meeting_service.pb.cc
    ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc/meeting_service.grpc.pb.cc
)

add_executable(
    vce_test_server
    ${VCE_TEST_SERVER_SOURCES}
)

target_compile_features(
    vce_test_server
    PRIVATE
        cxx_std_20
)

target_include_directories(
    vce_test_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${VCE_PROJECT_ROOT}/VideoConferenceEngine/private/core/service/rpc
)

target_link_libraries(
    vce_test_server
    PRIVATE
        protobuf::libprotobuf
        gRPC::grpc++
        Threads::Threads
)

set_target_properties(
    vce_test_server
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)