#include "InMemoryMeetingStore.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

std::int64_t CurrentUnixTimeSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

namespace VCE::TEST_SERVER {

// ==================== MeetingEventSubscription ====================

MeetingEventSubscription::MeetingEventSubscription(
    std::string meeting_id,
    std::string user_name)
    : m_meeting_id(std::move(meeting_id)),
      m_user_name(std::move(user_name))
{
}

bool MeetingEventSubscription::WaitNext(
    MeetingEventRecord& event,
    std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_condition.wait_for(lock, timeout, [this]() {
        return !m_events.empty() || m_closed;
    });

    /*
     * 订阅关闭时仍然优先返回已经排队的事件。
     * 这样EndMeeting可以先推送结束事件，再关闭事件流。
     */
    if (m_events.empty()) {
        return false;
    }

    event = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

bool MeetingEventSubscription::IsClosed() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

void MeetingEventSubscription::Push(MeetingEventRecord event)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_closed) {
            return;
        }

        m_events.emplace_back(std::move(event));
    }

    m_condition.notify_one();
}

void MeetingEventSubscription::Close()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_closed) {
            return;
        }

        m_closed = true;
    }

    m_condition.notify_all();
}

// ==================== InMemoryMeetingStore ====================

InMemoryMeetingStore::~InMemoryMeetingStore()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [meeting_id, subscriptions] : m_subscriptions) {
        (void)meeting_id;

        for (const auto& weak_subscription : subscriptions) {
            if (auto subscription = weak_subscription.lock()) {
                subscription->Close();
            }
        }
    }

    m_subscriptions.clear();
}

StoreResult InMemoryMeetingStore::RegisterUser(
    const std::string& user_name,
    const std::string& password)
{
    if (user_name.empty() || password.empty() ||
        user_name.size() > 64 || password.size() > 256) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "invalid user name or password");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_users.find(user_name) != m_users.end()) {
        return StoreResult::Failure(
            StoreError::kUserAlreadyExists,
            "user already exists");
    }

    /*
     * 仅用于本地联调，因此暂存明文密码。
     * 该存储方式不能用于生产环境。
     */
    m_users.emplace(user_name, password);
    return StoreResult::Success();
}

StoreResult InMemoryMeetingStore::LoginUser(
    const std::string& user_name,
    const std::string& password)
{
    if (user_name.empty() || password.empty()) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "user name or password is empty");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    const auto user_it = m_users.find(user_name);

    if (user_it == m_users.end()) {
        return StoreResult::Failure(
            StoreError::kUserNotFound,
            "user does not exist");
    }

    if (user_it->second != password) {
        return StoreResult::Failure(
            StoreError::kInvalidPassword,
            "incorrect password");
    }

    m_logged_in_users.insert(user_name);
    return StoreResult::Success();
}

StoreResult InMemoryMeetingStore::CreateMeeting(
    const ParticipantRecord& creator,
    const std::string& title,
    const std::string& description,
    std::int64_t start_time,
    MeetingRecord& meeting)
{
    meeting = {};

    if (creator.user_name.empty() || creator.client_ip == 0 ||
        title.empty() || title.size() > 256 ||
        description.size() > 2048) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "invalid meeting information");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!IsUserLoggedInLocked(creator.user_name)) {
        return StoreResult::Failure(
            StoreError::kNotLoggedIn,
            "user is not logged in");
    }

    if (m_user_meetings.find(creator.user_name) !=
        m_user_meetings.end()) {
        return StoreResult::Failure(
            StoreError::kAlreadyInMeeting,
            "user is already in a meeting");
    }

    MeetingRecord new_meeting;
    new_meeting.meeting_id = GenerateMeetingIdLocked();
    new_meeting.title = title;
    new_meeting.description = description;
    new_meeting.start_time =
        start_time > 0 ? start_time : CurrentUnixTimeSeconds();
    new_meeting.creator = creator;
    new_meeting.participants.emplace_back(creator);
    new_meeting.active = true;

    const std::string meeting_id = new_meeting.meeting_id;

    m_meetings.emplace(meeting_id, new_meeting);
    m_user_meetings[creator.user_name] = meeting_id;

    meeting = std::move(new_meeting);
    return StoreResult::Success();
}

StoreResult InMemoryMeetingStore::JoinMeeting(
    const std::string& meeting_id,
    const ParticipantRecord& participant,
    MeetingRecord& meeting)
{
    meeting = {};

    if (meeting_id.empty() || participant.user_name.empty() ||
        participant.client_ip == 0) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "invalid join meeting information");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!IsUserLoggedInLocked(participant.user_name)) {
        return StoreResult::Failure(
            StoreError::kNotLoggedIn,
            "user is not logged in");
    }

    auto meeting_it = m_meetings.find(meeting_id);

    if (meeting_it == m_meetings.end()) {
        return StoreResult::Failure(
            StoreError::kMeetingNotFound,
            "meeting does not exist");
    }

    MeetingRecord& target_meeting = meeting_it->second;

    if (!target_meeting.active) {
        return StoreResult::Failure(
            StoreError::kMeetingEnded,
            "meeting has ended");
    }

    const auto membership_it =
        m_user_meetings.find(participant.user_name);

    if (membership_it != m_user_meetings.end() &&
        membership_it->second != meeting_id) {
        return StoreResult::Failure(
            StoreError::kAlreadyInMeeting,
            "user is already in another meeting");
    }

    auto participant_it =
        FindParticipant(target_meeting.participants,
                        participant.user_name);

    /*
     * JoinMeeting允许安全重试。
     * 用户已经位于该会议时更新其IP并直接返回当前状态，
     * 不重复产生用户加入事件。
     */
    if (participant_it != target_meeting.participants.end()) {
        participant_it->client_ip = participant.client_ip;
        m_user_meetings[participant.user_name] = meeting_id;
        meeting = target_meeting;
        return StoreResult::Success();
    }

    target_meeting.participants.emplace_back(participant);
    m_user_meetings[participant.user_name] = meeting_id;
    meeting = target_meeting;

    MeetingEventRecord event;
    event.kind = MeetingEventKind::kUserJoined;
    event.meeting_id = meeting_id;
    event.trigger_user = participant;
    event.current_participants = target_meeting.participants;
    event.message = "user joined meeting";

    PublishEventLocked(event, false);
    return StoreResult::Success();
}

StoreResult InMemoryMeetingStore::LeaveMeeting(
    const std::string& meeting_id,
    const std::string& user_name,
    MeetingRecord& meeting)
{
    meeting = {};

    if (meeting_id.empty() || user_name.empty()) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "meeting ID or user name is empty");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!IsUserLoggedInLocked(user_name)) {
        return StoreResult::Failure(
            StoreError::kNotLoggedIn,
            "user is not logged in");
    }

    auto meeting_it = m_meetings.find(meeting_id);

    if (meeting_it == m_meetings.end()) {
        return StoreResult::Failure(
            StoreError::kMeetingNotFound,
            "meeting does not exist");
    }

    MeetingRecord& target_meeting = meeting_it->second;

    if (!target_meeting.active) {
        return StoreResult::Failure(
            StoreError::kMeetingEnded,
            "meeting has ended");
    }

    auto participant_it =
        FindParticipant(target_meeting.participants, user_name);

    if (participant_it == target_meeting.participants.end()) {
        return StoreResult::Failure(
            StoreError::kNotParticipant,
            "user is not a meeting participant");
    }

    const ParticipantRecord trigger_user = *participant_it;

    target_meeting.participants.erase(participant_it);
    m_user_meetings.erase(user_name);

    if (target_meeting.participants.empty()) {
        /*
         * 最后一名成员离开后自动关闭会议，
         * 避免本地测试过程中积累无法再使用的空会议。
         */
        target_meeting.active = false;
        target_meeting.end_time = CurrentUnixTimeSeconds();
        meeting = target_meeting;

        MeetingEventRecord event;
        event.kind = MeetingEventKind::kMeetingEnded;
        event.meeting_id = meeting_id;
        event.trigger_user = trigger_user;
        event.message = "meeting ended because the last participant left";

        PublishEventLocked(event, true);
        return StoreResult::Success();
    }

    meeting = target_meeting;

    MeetingEventRecord event;
    event.kind = MeetingEventKind::kUserLeft;
    event.meeting_id = meeting_id;
    event.trigger_user = trigger_user;
    event.current_participants = target_meeting.participants;
    event.message = "user left meeting";

    /*
     * 先向所有订阅者推送离开事件，再关闭离开用户自己的事件流。
     */
    PublishEventLocked(event, false);
    CloseUserSubscriptionsLocked(meeting_id, user_name);
    return StoreResult::Success();
}

StoreResult InMemoryMeetingStore::EndMeeting(
    const std::string& meeting_id,
    const std::string& user_name,
    MeetingRecord& meeting)
{
    meeting = {};

    if (meeting_id.empty() || user_name.empty()) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "meeting ID or user name is empty");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!IsUserLoggedInLocked(user_name)) {
        return StoreResult::Failure(
            StoreError::kNotLoggedIn,
            "user is not logged in");
    }

    auto meeting_it = m_meetings.find(meeting_id);

    if (meeting_it == m_meetings.end()) {
        return StoreResult::Failure(
            StoreError::kMeetingNotFound,
            "meeting does not exist");
    }

    MeetingRecord& target_meeting = meeting_it->second;

    if (!target_meeting.active) {
        return StoreResult::Failure(
            StoreError::kMeetingEnded,
            "meeting has already ended");
    }

    if (target_meeting.creator.user_name != user_name) {
        return StoreResult::Failure(
            StoreError::kPermissionDenied,
            "only the meeting creator can end the meeting");
    }

    const ParticipantRecord trigger_user = target_meeting.creator;

    for (const ParticipantRecord& participant :
         target_meeting.participants) {
        m_user_meetings.erase(participant.user_name);
    }

    target_meeting.participants.clear();
    target_meeting.active = false;
    target_meeting.end_time = CurrentUnixTimeSeconds();
    meeting = target_meeting;

    MeetingEventRecord event;
    event.kind = MeetingEventKind::kMeetingEnded;
    event.meeting_id = meeting_id;
    event.trigger_user = trigger_user;
    event.message = "meeting ended by creator";

    /*
     * 结束事件入队后关闭全部订阅。
     * WaitNext()仍会先返回已排队的结束事件。
     */
    PublishEventLocked(event, true);
    return StoreResult::Success();
}

StoreResult InMemoryMeetingStore::GetMeetingList(
    const std::string& user_name,
    std::int32_t page_size,
    std::int32_t page_number,
    std::vector<MeetingRecord>& meetings,
    std::int32_t& total_count,
    std::int32_t& total_pages) const
{
    meetings.clear();
    total_count = 0;
    total_pages = 1;

    if (user_name.empty() || page_size <= 0 ||
        page_size > 100 || page_number <= 0) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "invalid meeting list request");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!IsUserLoggedInLocked(user_name)) {
        return StoreResult::Failure(
            StoreError::kNotLoggedIn,
            "user is not logged in");
    }

    std::vector<MeetingRecord> ordered_meetings;
    ordered_meetings.reserve(m_meetings.size());

    for (const auto& [meeting_id, meeting] : m_meetings) {
        (void)meeting_id;
        ordered_meetings.emplace_back(meeting);
    }

    std::sort(
        ordered_meetings.begin(),
        ordered_meetings.end(),
        [](const MeetingRecord& left,
           const MeetingRecord& right) {
            if (left.start_time != right.start_time) {
                return left.start_time > right.start_time;
            }

            return left.meeting_id > right.meeting_id;
        });

    total_count =
        static_cast<std::int32_t>(ordered_meetings.size());

    if (total_count > 0) {
        total_pages =
            (total_count + page_size - 1) / page_size;
    }

    const std::int64_t first_index =
        static_cast<std::int64_t>(page_number - 1) *
        static_cast<std::int64_t>(page_size);

    if (first_index >= total_count) {
        return StoreResult::Success();
    }

    const std::int64_t last_index =
        std::min<std::int64_t>(
            first_index + page_size,
            total_count);

    meetings.reserve(
        static_cast<std::size_t>(
            last_index - first_index));

    for (std::int64_t index = first_index;
         index < last_index;
         ++index) {
        meetings.emplace_back(
            ordered_meetings[
                static_cast<std::size_t>(index)]);
    }

    return StoreResult::Success();
}

StoreResult InMemoryMeetingStore::SubscribeMeetingEvents(
    const std::string& meeting_id,
    const std::string& user_name,
    std::shared_ptr<MeetingEventSubscription>& subscription)
{
    subscription.reset();

    if (meeting_id.empty() || user_name.empty()) {
        return StoreResult::Failure(
            StoreError::kInvalidArgument,
            "meeting ID or user name is empty");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!IsUserLoggedInLocked(user_name)) {
        return StoreResult::Failure(
            StoreError::kNotLoggedIn,
            "user is not logged in");
    }

    auto meeting_it = m_meetings.find(meeting_id);

    if (meeting_it == m_meetings.end()) {
        return StoreResult::Failure(
            StoreError::kMeetingNotFound,
            "meeting does not exist");
    }

    if (!meeting_it->second.active) {
        return StoreResult::Failure(
            StoreError::kMeetingEnded,
            "meeting has ended");
    }

    if (FindParticipant(
            meeting_it->second.participants,
            user_name) ==
        meeting_it->second.participants.end()) {
        return StoreResult::Failure(
            StoreError::kNotParticipant,
            "user is not a meeting participant");
    }

    /*
     * 同一用户重连事件流时关闭旧订阅，
     * 保证每个用户只有一个活动事件队列。
     */
    CloseUserSubscriptionsLocked(meeting_id, user_name);
    RemoveExpiredSubscriptionsLocked(meeting_id);

    subscription =
        std::shared_ptr<MeetingEventSubscription>(
            new MeetingEventSubscription(
                meeting_id,
                user_name));

    m_subscriptions[meeting_id].emplace_back(subscription);
    return StoreResult::Success();
}

void InMemoryMeetingStore::UnsubscribeMeetingEvents(
    const std::shared_ptr<MeetingEventSubscription>& subscription)
{
    if (!subscription) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    const std::string& meeting_id =
        subscription->GetMeetingId();

    auto subscriptions_it =
        m_subscriptions.find(meeting_id);

    if (subscriptions_it != m_subscriptions.end()) {
        auto& subscriptions = subscriptions_it->second;

        subscriptions.erase(
            std::remove_if(
                subscriptions.begin(),
                subscriptions.end(),
                [&subscription](const auto& weak_subscription) {
                    const auto current =
                        weak_subscription.lock();

                    return !current ||
                           current == subscription;
                }),
            subscriptions.end());

        if (subscriptions.empty()) {
            m_subscriptions.erase(subscriptions_it);
        }
    }

    subscription->Close();
}

bool InMemoryMeetingStore::IsUserLoggedInLocked(
    const std::string& user_name) const
{
    return m_logged_in_users.find(user_name) !=
           m_logged_in_users.end();
}

std::string InMemoryMeetingStore::GenerateMeetingIdLocked()
{
    for (;;) {
        std::ostringstream stream;
        stream << "meeting-"
               << std::setw(6)
               << std::setfill('0')
               << m_next_meeting_number++;

        std::string meeting_id = stream.str();

        if (m_meetings.find(meeting_id) ==
            m_meetings.end()) {
            return meeting_id;
        }
    }
}

std::vector<ParticipantRecord>::iterator
InMemoryMeetingStore::FindParticipant(
    std::vector<ParticipantRecord>& participants,
    const std::string& user_name)
{
    return std::find_if(
        participants.begin(),
        participants.end(),
        [&user_name](const ParticipantRecord& participant) {
            return participant.user_name == user_name;
        });
}

std::vector<ParticipantRecord>::const_iterator
InMemoryMeetingStore::FindParticipant(
    const std::vector<ParticipantRecord>& participants,
    const std::string& user_name)
{
    return std::find_if(
        participants.begin(),
        participants.end(),
        [&user_name](const ParticipantRecord& participant) {
            return participant.user_name == user_name;
        });
}

void InMemoryMeetingStore::PublishEventLocked(
    const MeetingEventRecord& event,
    bool close_all_subscriptions)
{
    auto subscriptions_it =
        m_subscriptions.find(event.meeting_id);

    if (subscriptions_it == m_subscriptions.end()) {
        return;
    }

    auto& subscriptions = subscriptions_it->second;

    for (auto it = subscriptions.begin();
         it != subscriptions.end();) {
        auto subscription = it->lock();

        if (!subscription) {
            it = subscriptions.erase(it);
            continue;
        }

        subscription->Push(event);

        if (close_all_subscriptions) {
            subscription->Close();
            it = subscriptions.erase(it);
        } else {
            ++it;
        }
    }

    if (subscriptions.empty()) {
        m_subscriptions.erase(subscriptions_it);
    }
}

void InMemoryMeetingStore::CloseUserSubscriptionsLocked(
    const std::string& meeting_id,
    const std::string& user_name)
{
    auto subscriptions_it =
        m_subscriptions.find(meeting_id);

    if (subscriptions_it == m_subscriptions.end()) {
        return;
    }

    auto& subscriptions = subscriptions_it->second;

    subscriptions.erase(
        std::remove_if(
            subscriptions.begin(),
            subscriptions.end(),
            [&user_name](const auto& weak_subscription) {
                auto subscription =
                    weak_subscription.lock();

                if (!subscription) {
                    return true;
                }

                if (subscription->GetUserName() ==
                    user_name) {
                    subscription->Close();
                    return true;
                }

                return false;
            }),
        subscriptions.end());

    if (subscriptions.empty()) {
        m_subscriptions.erase(subscriptions_it);
    }
}

void InMemoryMeetingStore::RemoveExpiredSubscriptionsLocked(
    const std::string& meeting_id)
{
    auto subscriptions_it =
        m_subscriptions.find(meeting_id);

    if (subscriptions_it == m_subscriptions.end()) {
        return;
    }

    auto& subscriptions = subscriptions_it->second;

    subscriptions.erase(
        std::remove_if(
            subscriptions.begin(),
            subscriptions.end(),
            [](const auto& weak_subscription) {
                return weak_subscription.expired();
            }),
        subscriptions.end());

    if (subscriptions.empty()) {
        m_subscriptions.erase(subscriptions_it);
    }
}

} // namespace VCE::TEST_SERVER