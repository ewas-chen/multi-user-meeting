#pragma once

#include "InMemoryMeetingStore.h"
#include "meeting_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace VCE::TEST_SERVER {

/**
 * @brief 本地测试用会议gRPC服务
 *
 * 负责实现：
 * - 创建会议；
 * - 加入会议；
 * - 离开会议；
 * - 结束会议；
 * - 查询会议列表；
 * - 订阅会议成员事件。
 *
 * push_media_server_ip和pull_media_server_ip使用相同的SRS地址，
 * 与当前客户端只支持单一RTC外部地址的实现保持一致。
 */
class MeetingServiceImpl final
    : public ::meeting_service::MeetingService::Service {
public:
    /**
     * @param store              UserServiceImpl共享的内存状态
     * @param media_server_ip    SRS服务器的网络字节序IPv4整数
     */
    MeetingServiceImpl(
        std::shared_ptr<InMemoryMeetingStore> store,
        std::uint32_t media_server_ip);

    ~MeetingServiceImpl() override = default;

    MeetingServiceImpl(const MeetingServiceImpl&) = delete;
    MeetingServiceImpl& operator=(const MeetingServiceImpl&) = delete;
    MeetingServiceImpl(MeetingServiceImpl&&) = delete;
    MeetingServiceImpl& operator=(MeetingServiceImpl&&) = delete;

    grpc::Status CreateMeeting(
        grpc::ServerContext* context,
        const ::meeting_service::CreateMeetingRequest* request,
        ::meeting_service::CreateMeetingResponse* response) override;

    grpc::Status JoinMeeting(
        grpc::ServerContext* context,
        const ::meeting_service::JoinMeetingRequest* request,
        ::meeting_service::JoinMeetingResponse* response) override;

    grpc::Status LeaveMeeting(
        grpc::ServerContext* context,
        const ::meeting_service::LeaveMeetingRequest* request,
        ::meeting_service::LeaveMeetingResponse* response) override;

    grpc::Status EndMeeting(
        grpc::ServerContext* context,
        const ::meeting_service::EndMeetingRequest* request,
        ::meeting_service::EndMeetingResponse* response) override;

    grpc::Status GetMeetingList(
        grpc::ServerContext* context,
        const ::meeting_service::GetMeetingListRequest* request,
        ::meeting_service::GetMeetingListResponse* response) override;

    grpc::Status SubscribeMeetingEvents(
        grpc::ServerContext* context,
        const ::meeting_service::SubscribeMeetingEventsRequest* request,
        grpc::ServerWriter<::meeting_service::MeetingEvent>* writer) override;

private:
    static ParticipantRecord ToParticipantRecord(
        const ::meeting_service::UserInfo& user);

    static ::meeting_service::UserInfo ToProtoUser(
        const ParticipantRecord& participant);

    ::meeting_service::ParticipantInfo ToProtoParticipant(
        const ParticipantRecord& participant) const;

    static void AppendUsers(
        const std::vector<ParticipantRecord>& participants,
        google::protobuf::RepeatedPtrField<
            ::meeting_service::UserInfo>* output);

    void AppendParticipantDetails(
        const std::vector<ParticipantRecord>& participants,
        google::protobuf::RepeatedPtrField<
            ::meeting_service::ParticipantInfo>* output) const;

    static void FillMeetingInfo(
        const MeetingRecord& meeting,
        ::meeting_service::MeetingInfo* output);

    void FillMeetingEvent(
        const MeetingEventRecord& event,
        ::meeting_service::MeetingEvent* output) const;

    static std::uint32_t BusinessErrorCode(
        StoreError error) noexcept;

private:
    static constexpr std::chrono::milliseconds
        kEventWaitTimeout{200};

    std::shared_ptr<InMemoryMeetingStore> m_store;

    /*
     * proto要求使用网络字节序IPv4整数。
     * 当前客户端要求推流和拉流服务器地址相同。
     */
    std::uint32_t m_media_server_ip{0};
};

} // namespace VCE::TEST_SERVER