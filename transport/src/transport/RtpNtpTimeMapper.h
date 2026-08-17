#pragma once

#include "TransportDefine.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace TRANSPORT {

/**
 * @brief 将单路媒体的RTP时间戳映射到RTCP NTP公共时间线
 *
 * 每个实例只负责一路媒体：
 *
 * - 视频实例使用90000Hz RTP时钟；
 * - Opus音频实例使用48000Hz RTP时钟。
 *
 * RTCP Sender Report提供一个同步锚点：
 *
 *     SR中的RTP时间戳 <-> SR中的NTP时间戳
 *
 * 得到同步锚点后，可以将任意媒体帧的RTP时间戳转换为：
 *
 *     frame_time_us =
 *         sr_ntp_time_us +
 *         (frame_rtp - sr_rtp) * 1000000 / clock_rate
 *
 * 本类不依赖libdatachannel类型，便于独立测试。
 */
class TRANSPORT_ENGINE_LOCAL RtpNtpTimeMapper final
{
public:
    enum class UpdateResult : std::uint8_t
    {
        // 接受了新的RTCP SR同步锚点
        kAccepted = 0,

        // 收到的同步锚点与当前锚点完全相同
        kUnchanged,

        // RTP、NTP或时钟频率无效
        kInvalid,

        /*
         * 新SR与当前时间线存在明显跳变。
         *
         * 可能原因：
         * - 发送端重启；
         * - RTP时间线重新开始；
         * - 媒体源发生切换；
         * - 收到异常RTCP SR。
         *
         * 返回该结果时不会替换旧锚点。
         * 调用方应重置相关音视频队列及同步状态，然后调用Reset()
         * 并重新提交该SR。
         */
        kDiscontinuity
    };

    /**
     * @param clock_rate RTP时钟频率
     *                   H.264通常为90000，Opus通常为48000
     *
     * @param max_sr_deviation_us
     *        新SR相对于当前映射时间线允许的最大偏差。
     *        超过该值认为时间线发生跳变。
     */
    explicit RtpNtpTimeMapper(
        std::uint32_t clock_rate,
        std::int64_t max_sr_deviation_us = 500'000);

    ~RtpNtpTimeMapper() = default;

    RtpNtpTimeMapper(const RtpNtpTimeMapper&) = delete;
    RtpNtpTimeMapper& operator=(const RtpNtpTimeMapper&) = delete;
    RtpNtpTimeMapper(RtpNtpTimeMapper&&) = delete;
    RtpNtpTimeMapper& operator=(RtpNtpTimeMapper&&) = delete;

    /**
     * @brief 更新RTCP Sender Report同步锚点
     *
     * 参数可以直接传入：
     *
     *     rtc::RtcpReceivingSession::getSyncTimestamps()
     *
     * 返回的rtpTimestamp类型虽然是uint64_t，但RTP时间戳本身只有
     * 32位，因此本函数会检查高32位是否为0。
     *
     * ntp_timestamp是RFC 3550定义的64位NTP定点时间：
     *
     *     高32位：秒
     *     低32位：一秒的小数部分
     *
     * 它不是Unix微秒时间，本类会在内部完成转换。
     */
    UpdateResult UpdateSenderReport(
        std::uint64_t rtp_timestamp,
        std::uint64_t ntp_timestamp);

    /**
     * @brief 将媒体帧RTP时间戳映射到NTP公共时间线
     *
     * @return 映射后的微秒时间；尚未收到有效SR时返回std::nullopt。
     *
     * 返回值以NTP时间线为基准，不是Unix时间。
     * 音视频同步只要求两路媒体处于同一时间线，不需要将其转换成
     * 本地日期时间。
     */
    std::optional<std::int64_t> MapToNtpTimeUs(
        std::uint32_t rtp_timestamp) const;

    /**
     * @brief 当前是否已经拥有有效RTCP SR同步锚点
     */
    bool IsReady() const;

    /**
     * @brief 清除当前同步锚点
     *
     * 用于RTC重连、发送端重启、媒体源切换或时间线跳变。
     */
    void Reset();

    std::uint32_t GetClockRate() const noexcept
    {
        return m_clock_rate;
    }

private:
    /**
     * @brief 将64位NTP定点时间转换成NTP时间线微秒数
     */
    static std::int64_t NtpTimestampToMicroseconds(
        std::uint64_t ntp_timestamp) noexcept;

    /**
     * @brief 计算两个32位RTP时间戳之间的有符号差值
     *
     * 转换为int32_t后可以自然处理一次32位RTP时间戳回绕。
     * RTCP SR会周期性到达，因此正常情况下不会跨越超过半个
     * RTP时间戳空间。
     */
    static std::int32_t CalculateRtpDelta(
        std::uint32_t current_timestamp,
        std::uint32_t base_timestamp) noexcept;

    /**
     * @brief 将RTP时钟增量转换为微秒
     */
    static std::int64_t RtpDeltaToMicroseconds(
        std::int32_t rtp_delta,
        std::uint32_t clock_rate) noexcept;

private:
    const std::uint32_t m_clock_rate;
    const std::int64_t m_max_sr_deviation_us;

    mutable std::mutex m_mutex;

    bool m_ready{false};

    // 最近一次接受的RTCP SR同步锚点
    std::uint32_t m_sr_rtp_timestamp{0};
    std::uint64_t m_sr_ntp_timestamp{0};
    std::int64_t m_sr_ntp_time_us{0};
};

} // namespace TRANSPORT