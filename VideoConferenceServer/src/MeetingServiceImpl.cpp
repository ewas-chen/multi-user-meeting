#include "MeetingServiceImpl.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

namespace {

std::int64_t CurrentUnixTimeSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

grpc::Status SubscriptionFailureStatus(
    const VCE::TEST_SERVER::StoreResult& result)
{
    using VCE::TEST_SERVER::StoreError;

    switch (result.error) {
    case StoreError::kInvalidArgument:
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            result.message);

    case StoreError::kUserNotFound:
    case StoreError::kNotLoggedIn:
        return grpc::Status(
            grpc::StatusCode::UNAUTHENTICATED,
            result.message);

    case StoreError::kMeetingNotFound:
        return grpc::Status(
            grpc::StatusCode::NOT_FOUND,
            result.message);

    case StoreError::kMeetingEnded:
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            result.message);

    case StoreError::kNotParticipant:
    case StoreError::kPermissionDenied:
        return grpc::Status(
            grpc::StatusCode::PERMISSION_DENIED,
            result.message);

    default:
        return grpc::Status(
            grpc::StatusCode::UNKNOWN,
            result.message);
    }
}

} // namespace

namespace VCE::TEST_SERVER {

MeetingServiceImpl::MeetingServiceImpl(
    std::shared_ptr<InMemoryMeetingStore> store,
    std::uint32_t media_server_ip)
    : m_store(std::move(store)),
      m_media_server_ip(media_server_ip)
{
}

grpc::Status MeetingServiceImpl::CreateMeeting(
    grpc::ServerContext* context,
    const ::meeting_service::CreateMeetingRequest* request,
    ::meeting_service::CreateMeetingResponse* response)
{
    (void)context;

    if (!request || !response) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "request or response is null");
    }

    if (!m_store || m_media_server_ip == 0) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store or media server is unavailable");
    }

    response->Clear();

    MeetingRecord meeting;

    const StoreResult result =
        m_store->CreateMeeting(
            ToParticipantRecord(request->user()),
            request->meetingtitle(),
            request->meetingdescription(),
            request->starttime(),
            meeting);

    response->set_errorcode(
        BusinessErrorCode(result.error));

    response->set_errormessage(result.message);

    if (!result.IsSuccess()) {
        std::cout
            << "[TestServer] Create meeting rejected: user="
            << request->user().username()
            << ", code="
            << response->errorcode()
            << ", message="
            << result.message
            << std::endl;

        return grpc::Status::OK;
    }

    response->set_meetingid(meeting.meeting_id);
    response->set_pushmeetingserverip(m_media_server_ip);
    response->set_pullmeetingserverip(m_media_server_ip);

    AppendUsers(
        meeting.participants,
        response->mutable_participants());

    std::cout
        << "[TestServer] Meeting created: id="
        << meeting.meeting_id
        << ", creator="
        << meeting.creator.user_name
        << std::endl;

    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::JoinMeeting(
    grpc::ServerContext* context,
    const ::meeting_service::JoinMeetingRequest* request,
    ::meeting_service::JoinMeetingResponse* response)
{
    (void)context;

    if (!request || !response) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "request or response is null");
    }

    if (!m_store || m_media_server_ip == 0) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store or media server is unavailable");
    }

    response->Clear();

    MeetingRecord meeting;

    const StoreResult result =
        m_store->JoinMeeting(
            request->meetingid(),
            ToParticipantRecord(request->user()),
            meeting);

    response->set_errorcode(
        BusinessErrorCode(result.error));

    response->set_errormessage(result.message);

    if (!result.IsSuccess()) {
        std::cout
            << "[TestServer] Join meeting rejected: meeting="
            << request->meetingid()
            << ", user="
            << request->user().username()
            << ", code="
            << response->errorcode()
            << ", message="
            << result.message
            << std::endl;

        return grpc::Status::OK;
    }

    response->set_pushmeetingserverip(m_media_server_ip);
    response->set_pullmeetingserverip(m_media_server_ip);
    response->set_meetingtitle(meeting.title);
    response->set_meetingdescription(meeting.description);

    AppendUsers(
        meeting.participants,
        response->mutable_participants());

    std::cout
        << "[TestServer] User joined meeting: meeting="
        << meeting.meeting_id
        << ", user="
        << request->user().username()
        << std::endl;

    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::LeaveMeeting(
    grpc::ServerContext* context,
    const ::meeting_service::LeaveMeetingRequest* request,
    ::meeting_service::LeaveMeetingResponse* response)
{
    (void)context;

    if (!request || !response) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "request or response is null");
    }

    if (!m_store) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store is unavailable");
    }

    response->Clear();

    MeetingRecord meeting;

    const StoreResult result =
        m_store->LeaveMeeting(
            request->meetingid(),
            request->user().username(),
            meeting);

    response->set_errorcode(
        BusinessErrorCode(result.error));

    response->set_errormessage(result.message);

    if (!result.IsSuccess()) {
        std::cout
            << "[TestServer] Leave meeting rejected: meeting="
            << request->meetingid()
            << ", user="
            << request->user().username()
            << ", code="
            << response->errorcode()
            << ", message="
            << result.message
            << std::endl;

        return grpc::Status::OK;
    }

    AppendUsers(
        meeting.participants,
        response->mutable_participants());

    std::cout
        << "[TestServer] User left meeting: meeting="
        << request->meetingid()
        << ", user="
        << request->user().username()
        << std::endl;

    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::EndMeeting(
    grpc::ServerContext* context,
    const ::meeting_service::EndMeetingRequest* request,
    ::meeting_service::EndMeetingResponse* response)
{
    (void)context;

    if (!request || !response) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "request or response is null");
    }

    if (!m_store) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store is unavailable");
    }

    response->Clear();

    MeetingRecord meeting;

    const StoreResult result =
        m_store->EndMeeting(
            request->meetingid(),
            request->user().username(),
            meeting);

    response->set_errorcode(
        BusinessErrorCode(result.error));

    response->set_errormessage(result.message);

    if (!result.IsSuccess()) {
        std::cout
            << "[TestServer] End meeting rejected: meeting="
            << request->meetingid()
            << ", user="
            << request->user().username()
            << ", code="
            << response->errorcode()
            << ", message="
            << result.message
            << std::endl;

        return grpc::Status::OK;
    }

    AppendUsers(
        meeting.participants,
        response->mutable_participants());

    std::cout
        << "[TestServer] Meeting ended: meeting="
        << request->meetingid()
        << ", creator="
        << request->user().username()
        << std::endl;

    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::GetMeetingList(
    grpc::ServerContext* context,
    const ::meeting_service::GetMeetingListRequest* request,
    ::meeting_service::GetMeetingListResponse* response)
{
    (void)context;

    if (!request || !response) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "request or response is null");
    }

    if (!m_store) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store is unavailable");
    }

    response->Clear();

    std::vector<MeetingRecord> meetings;
    std::int32_t total_count = 0;
    std::int32_t total_pages = 1;

    const StoreResult result =
        m_store->GetMeetingList(
            request->user().username(),
            request->pagesize(),
            request->pagenumber(),
            meetings,
            total_count,
            total_pages);

    response->set_errorcode(
        BusinessErrorCode(result.error));

    response->set_errormessage(result.message);

    if (!result.IsSuccess()) {
        return grpc::Status::OK;
    }

    response->set_totalcount(total_count);
    response->set_totalpages(total_pages);

    for (const MeetingRecord& meeting : meetings) {
        FillMeetingInfo(
            meeting,
            response->add_meetings());
    }

    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::SubscribeMeetingEvents(
    grpc::ServerContext* context,
    const ::meeting_service::SubscribeMeetingEventsRequest* request,
    grpc::ServerWriter<::meeting_service::MeetingEvent>* writer)
{
    if (!context || !request || !writer) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "context, request or writer is null");
    }

    if (!m_store) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "meeting store is unavailable");
    }

    std::shared_ptr<MeetingEventSubscription> subscription;

    const StoreResult result =
        m_store->SubscribeMeetingEvents(
            request->meeting_id(),
            request->user().username(),
            subscription);

    if (!result.IsSuccess()) {
        return SubscriptionFailureStatus(result);
    }

    std::cout
        << "[TestServer] Event stream opened: meeting="
        << request->meeting_id()
        << ", user="
        << request->user().username()
        << std::endl;

    while (!context->IsCancelled()) {
        MeetingEventRecord event;

        if (!subscription->WaitNext(
                event,
                kEventWaitTimeout)) {
            if (subscription->IsClosed()) {
                break;
            }

            continue;
        }

        ::meeting_service::MeetingEvent proto_event;
        FillMeetingEvent(event, &proto_event);

        if (!writer->Write(proto_event)) {
            break;
        }

        if (event.kind ==
            MeetingEventKind::kMeetingEnded) {
            break;
        }
    }

    m_store->UnsubscribeMeetingEvents(subscription);

    std::cout
        << "[TestServer] Event stream closed: meeting="
        << request->meeting_id()
        << ", user="
        << request->user().username()
        << std::endl;

    return grpc::Status::OK;
}

ParticipantRecord MeetingServiceImpl::ToParticipantRecord(
    const ::meeting_service::UserInfo& user)
{
    ParticipantRecord participant;
    participant.user_name = user.username();
    participant.client_ip = user.clientip();
    return participant;
}

::meeting_service::UserInfo MeetingServiceImpl::ToProtoUser(
    const ParticipantRecord& participant)
{
    ::meeting_service::UserInfo user;
    user.set_username(participant.user_name);
    user.set_clientip(participant.client_ip);
    return user;
}

::meeting_service::ParticipantInfo
MeetingServiceImpl::ToProtoParticipant(
    const ParticipantRecord& participant) const
{
    ::meeting_service::ParticipantInfo output;
    *output.mutable_user() = ToProtoUser(participant);
    output.set_pushmeetingserverip(m_media_server_ip);
    output.set_pullmeetingserverip(m_media_server_ip);
    return output;
}

void MeetingServiceImpl::AppendUsers(
    const std::vector<ParticipantRecord>& participants,
    google::protobuf::RepeatedPtrField<
        ::meeting_service::UserInfo>* output)
{
    if (!output) {
        return;
    }

    output->Clear();
    output->Reserve(
        static_cast<int>(participants.size()));

    for (const ParticipantRecord& participant :
         participants) {
        *output->Add() = ToProtoUser(participant);
    }
}

void MeetingServiceImpl::AppendParticipantDetails(
    const std::vector<ParticipantRecord>& participants,
    google::protobuf::RepeatedPtrField<
        ::meeting_service::ParticipantInfo>* output) const
{
    if (!output) {
        return;
    }

    output->Clear();
    output->Reserve(
        static_cast<int>(participants.size()));

    for (const ParticipantRecord& participant :
         participants) {
        *output->Add() =
            ToProtoParticipant(participant);
    }
}

void MeetingServiceImpl::FillMeetingInfo(
    const MeetingRecord& meeting,
    ::meeting_service::MeetingInfo* output)
{
    if (!output) {
        return;
    }

    output->Clear();

    output->set_meetingid(meeting.meeting_id);
    output->set_meetingtitle(meeting.title);
    output->set_meetingdescription(meeting.description);
    output->set_starttime(meeting.start_time);

    const std::int64_t finish_time =
        meeting.active
            ? CurrentUnixTimeSeconds()
            : meeting.end_time;

    const std::int64_t duration =
        finish_time > meeting.start_time
            ? finish_time - meeting.start_time
            : 0;

    output->set_duration(duration);
    *output->mutable_creator() =
        ToProtoUser(meeting.creator);

    output->set_participantcount(
        static_cast<std::int32_t>(
            meeting.participants.size()));

    output->set_isactive(meeting.active);
}

void MeetingServiceImpl::FillMeetingEvent(
    const MeetingEventRecord& event,
    ::meeting_service::MeetingEvent* output) const
{
    if (!output) {
        return;
    }

    output->Clear();

    switch (event.kind) {
    case MeetingEventKind::kUserJoined:
        output->set_event_type(
            ::meeting_service::EVENT_USER_JOINED);
        break;

    case MeetingEventKind::kUserLeft:
        output->set_event_type(
            ::meeting_service::EVENT_USER_LEFT);
        break;

    case MeetingEventKind::kMeetingEnded:
        output->set_event_type(
            ::meeting_service::EVENT_MEETING_ENDED);
        break;
    }

    output->set_meeting_id(event.meeting_id);

    *output->mutable_trigger_user() =
        ToProtoUser(event.trigger_user);

    AppendParticipantDetails(
        event.current_participants,
        output->mutable_current_participants());

    output->set_message(event.message);
}

std::uint32_t MeetingServiceImpl::BusinessErrorCode(
    StoreError error) noexcept
{
    switch (error) {
    case StoreError::kSuccess:
        return 0;

    case StoreError::kInvalidArgument:
        return 3001;

    case StoreError::kUserAlreadyExists:
        return 3002;

    case StoreError::kUserNotFound:
        return 3003;

    case StoreError::kInvalidPassword:
        return 3004;

    case StoreError::kNotLoggedIn:
        return 3005;

    case StoreError::kMeetingNotFound:
        return 3101;

    case StoreError::kMeetingEnded:
        return 3102;

    case StoreError::kAlreadyInMeeting:
        return 3103;

    case StoreError::kNotParticipant:
        return 3104;

    case StoreError::kPermissionDenied:
        return 3105;
    }

    return 3999;
}

} // namespace VCE::TEST_SERVER