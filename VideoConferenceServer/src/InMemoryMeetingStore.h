#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace VCE::TEST_SERVER {

enum class StoreError {
    kSuccess = 0,
    kInvalidArgument,
    kUserAlreadyExists,
    kUserNotFound,
    kInvalidPassword,
    kNotLoggedIn,
    kMeetingNotFound,
    kMeetingEnded,
    kAlreadyInMeeting,
    kNotParticipant,
    kPermissionDenied
};

struct StoreResult final {
    StoreError error{StoreError::kSuccess};
    std::string message{"success"};

    [[nodiscard]]
    bool IsSuccess() const noexcept {
        return error == StoreError::kSuccess;
    }

    static StoreResult Success() {
        return {};
    }

    static StoreResult Failure(StoreError error, std::string message) {
        return {error, std::move(message)};
    }
};

/**
 * @brief 服务端内部保存的参会者信息
 *
 * client_ip保持proto使用的网络字节序IPv4整数。
 */
struct ParticipantRecord final {
    std::string user_name;
    std::uint32_t client_ip{0};
};

struct MeetingRecord final {
    std::string meeting_id;
    std::string title;
    std::string description;

    std::int64_t start_time{0};
    std::int64_t end_time{0};

    ParticipantRecord creator;
    std::vector<ParticipantRecord> participants;

    bool active{false};
};

enum class MeetingEventKind {
    kUserJoined,
    kUserLeft,
    kMeetingEnded
};

/**
 * @brief 与proto无关的内部会议事件
 *
 * MeetingServiceImpl负责将它转换成meeting_service::MeetingEvent。
 */
struct MeetingEventRecord final {
    MeetingEventKind kind{MeetingEventKind::kUserJoined};
    std::string meeting_id;
    ParticipantRecord trigger_user;
    std::vector<ParticipantRecord> current_participants;
    std::string message;
};

class InMemoryMeetingStore;

/**
 * @brief 单个SubscribeMeetingEvents调用对应的事件队列
 *
 * gRPC流线程通过WaitNext()阻塞等待事件。等待使用有限超时，
 * 使服务线程能够定期检查ServerContext是否已取消。
 */
class MeetingEventSubscription final {
public:
    MeetingEventSubscription(const MeetingEventSubscription&) = delete;
    MeetingEventSubscription& operator=(const MeetingEventSubscription&) = delete;
    MeetingEventSubscription(MeetingEventSubscription&&) = delete;
    MeetingEventSubscription& operator=(MeetingEventSubscription&&) = delete;

    bool WaitNext(MeetingEventRecord& event,
                  std::chrono::milliseconds timeout);

    [[nodiscard]]
    bool IsClosed() const noexcept;

    [[nodiscard]]
    const std::string& GetMeetingId() const noexcept {
        return m_meeting_id;
    }

    [[nodiscard]]
    const std::string& GetUserName() const noexcept {
        return m_user_name;
    }

private:
    friend class InMemoryMeetingStore;

    MeetingEventSubscription(std::string meeting_id,
                             std::string user_name);

    void Push(MeetingEventRecord event);
    void Close();

private:
    std::string m_meeting_id;
    std::string m_user_name;

    std::deque<MeetingEventRecord> m_events;

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_closed{false};
};

/**
 * @brief 本地测试服务端的线程安全状态中心
 *
 * UserServiceImpl和MeetingServiceImpl共享同一个实例。
 * 所有用户、会议、成员关系和事件订阅均保存在内存中。
 *
 * 该实现只用于客户端联调：
 * - 密码以明文暂存在内存中；
 * - 不提供Token和会话过期；
 * - 不提供数据库持久化；
 * - 不用于生产环境。
 */
class InMemoryMeetingStore final {
public:
    InMemoryMeetingStore() = default;
    ~InMemoryMeetingStore();

    InMemoryMeetingStore(const InMemoryMeetingStore&) = delete;
    InMemoryMeetingStore& operator=(const InMemoryMeetingStore&) = delete;
    InMemoryMeetingStore(InMemoryMeetingStore&&) = delete;
    InMemoryMeetingStore& operator=(InMemoryMeetingStore&&) = delete;

    // ==================== 用户状态 ====================

    StoreResult RegisterUser(const std::string& user_name,
                             const std::string& password);

    StoreResult LoginUser(const std::string& user_name,
                          const std::string& password);

    // ==================== 会议状态 ====================

    StoreResult CreateMeeting(const ParticipantRecord& creator,
                              const std::string& title,
                              const std::string& description,
                              std::int64_t start_time,
                              MeetingRecord& meeting);

    StoreResult JoinMeeting(const std::string& meeting_id,
                            const ParticipantRecord& participant,
                            MeetingRecord& meeting);

    StoreResult LeaveMeeting(const std::string& meeting_id,
                             const std::string& user_name,
                             MeetingRecord& meeting);

    StoreResult EndMeeting(const std::string& meeting_id,
                           const std::string& user_name,
                           MeetingRecord& meeting);

    StoreResult GetMeetingList(const std::string& user_name,
                               std::int32_t page_size,
                               std::int32_t page_number,
                               std::vector<MeetingRecord>& meetings,
                               std::int32_t& total_count,
                               std::int32_t& total_pages) const;

    // ==================== 会议事件订阅 ====================

    StoreResult SubscribeMeetingEvents(
        const std::string& meeting_id,
        const std::string& user_name,
        std::shared_ptr<MeetingEventSubscription>& subscription);

    void UnsubscribeMeetingEvents(
        const std::shared_ptr<MeetingEventSubscription>& subscription);

private:
    [[nodiscard]]
    bool IsUserLoggedInLocked(const std::string& user_name) const;

    [[nodiscard]]
    std::string GenerateMeetingIdLocked();

    static std::vector<ParticipantRecord>::iterator FindParticipant(
        std::vector<ParticipantRecord>& participants,
        const std::string& user_name);

    static std::vector<ParticipantRecord>::const_iterator FindParticipant(
        const std::vector<ParticipantRecord>& participants,
        const std::string& user_name);

    void PublishEventLocked(const MeetingEventRecord& event,
                            bool close_all_subscriptions);

    void CloseUserSubscriptionsLocked(const std::string& meeting_id,
                                      const std::string& user_name);

    void RemoveExpiredSubscriptionsLocked(const std::string& meeting_id);

private:
    mutable std::mutex m_mutex;

    // 本地测试服务端只保存明文密码，不用于生产环境
    std::unordered_map<std::string, std::string> m_users;
    std::unordered_set<std::string> m_logged_in_users;

    std::unordered_map<std::string, MeetingRecord> m_meetings;

    // 限制同一用户同时只加入一个会议
    std::unordered_map<std::string, std::string> m_user_meetings;

    std::unordered_map<
        std::string,
        std::vector<std::weak_ptr<MeetingEventSubscription>>>
        m_subscriptions;

    std::uint64_t m_next_meeting_number{1};
};

} // namespace VCE::TEST_SERVER