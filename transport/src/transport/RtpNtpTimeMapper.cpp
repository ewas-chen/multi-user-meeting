#include "RtpNtpTimeMapper.h"

#include <limits>

namespace TRANSPORT {

RtpNtpTimeMapper::RtpNtpTimeMapper(
    std::uint32_t clock_rate,
    std::int64_t max_sr_deviation_us)
    : m_clock_rate(clock_rate),
      m_max_sr_deviation_us(max_sr_deviation_us)
{
}

RtpNtpTimeMapper::UpdateResult
RtpNtpTimeMapper::UpdateSenderReport(
    std::uint64_t rtp_timestamp,
    std::uint64_t ntp_timestamp)
{
    /*
     * libdatachannel使用uint64_t返回RTP同步时间戳，
     * 但标准RTP时间戳本身只有32位。
     */
    if (m_clock_rate == 0 ||
        m_max_sr_deviation_us <= 0 ||
        rtp_timestamp >
            std::numeric_limits<std::uint32_t>::max() ||
        ntp_timestamp == 0) {
        return UpdateResult::kInvalid;
    }

    const auto new_rtp_timestamp =
        static_cast<std::uint32_t>(rtp_timestamp);

    const std::int64_t new_ntp_time_us =
        NtpTimestampToMicroseconds(ntp_timestamp);

    std::lock_guard<std::mutex> lock(m_mutex);

    /*
     * 第一次收到有效RTCP SR时直接建立同步锚点。
     */
    if (!m_ready) {
        m_sr_rtp_timestamp = new_rtp_timestamp;
        m_sr_ntp_timestamp = ntp_timestamp;
        m_sr_ntp_time_us = new_ntp_time_us;
        m_ready = true;

        return UpdateResult::kAccepted;
    }

    /*
     * libdatachannel可能在多个媒体帧之间返回同一组最新SR，
     * 相同同步点不需要重复更新。
     */
    if (m_sr_rtp_timestamp == new_rtp_timestamp &&
        m_sr_ntp_timestamp == ntp_timestamp) {
        return UpdateResult::kUnchanged;
    }

    /*
     * 使用当前锚点预测新SR中的RTP时间戳应当对应的NTP时间。
     *
     * 如果新SR与预测结果相差过大，通常说明发送端重启、
     * 媒体源切换或时间线发生跳变。
     */
    const std::int32_t rtp_delta =
        CalculateRtpDelta(
            new_rtp_timestamp,
            m_sr_rtp_timestamp);

    const std::int64_t expected_ntp_time_us =
        m_sr_ntp_time_us +
        RtpDeltaToMicroseconds(
            rtp_delta,
            m_clock_rate);

    const std::int64_t deviation_us =
        new_ntp_time_us >= expected_ntp_time_us
            ? new_ntp_time_us - expected_ntp_time_us
            : expected_ntp_time_us - new_ntp_time_us;

    if (deviation_us > m_max_sr_deviation_us) {
        /*
         * 保留旧锚点，让调用方决定何时清空音视频缓存并重建时间线。
         */
        return UpdateResult::kDiscontinuity;
    }

    /*
     * 使用更新的SR作为新锚点，可以降低长时间从旧锚点外推
     * 产生的换算误差，并为后续时钟漂移分析保留基础。
     */
    m_sr_rtp_timestamp = new_rtp_timestamp;
    m_sr_ntp_timestamp = ntp_timestamp;
    m_sr_ntp_time_us = new_ntp_time_us;

    return UpdateResult::kAccepted;
}

std::optional<std::int64_t>
RtpNtpTimeMapper::MapToNtpTimeUs(
    std::uint32_t rtp_timestamp) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_ready || m_clock_rate == 0) {
        return std::nullopt;
    }

    const std::int32_t rtp_delta =
        CalculateRtpDelta(
            rtp_timestamp,
            m_sr_rtp_timestamp);

    return m_sr_ntp_time_us +
           RtpDeltaToMicroseconds(
               rtp_delta,
               m_clock_rate);
}

bool RtpNtpTimeMapper::IsReady() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ready;
}

void RtpNtpTimeMapper::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_ready = false;
    m_sr_rtp_timestamp = 0;
    m_sr_ntp_timestamp = 0;
    m_sr_ntp_time_us = 0;
}

std::int64_t
RtpNtpTimeMapper::NtpTimestampToMicroseconds(
    std::uint64_t ntp_timestamp) noexcept
{
    /*
     * NTP 64位定点格式：
     *
     * 高32位：从NTP纪元开始经过的秒数；
     * 低32位：当前秒的小数部分。
     */
    const std::uint32_t seconds =
        static_cast<std::uint32_t>(
            ntp_timestamp >> 32U);

    const std::uint32_t fraction =
        static_cast<std::uint32_t>(
            ntp_timestamp & 0xFFFFFFFFULL);

    const std::int64_t seconds_us =
        static_cast<std::int64_t>(seconds) *
        1'000'000LL;

    /*
     * fraction / 2^32表示一秒的小数部分。
     * 使用整数运算，避免浮点精度和不同平台结果差异。
     */
    const std::uint64_t fraction_us =
        (static_cast<std::uint64_t>(fraction) *
         1'000'000ULL) >>
        32U;

    return seconds_us +
           static_cast<std::int64_t>(fraction_us);
}

std::int32_t
RtpNtpTimeMapper::CalculateRtpDelta(
    std::uint32_t current_timestamp,
    std::uint32_t base_timestamp) noexcept
{
    /*
     * 无符号减法首先按照模2^32计算，再解释为有符号32位差值。
     *
     * 例如：
     *
     * base    = 0xFFFFFF00
     * current = 0x00000100
     *
     * 结果为+512，而不是一个巨大的负数。
     */
    return static_cast<std::int32_t>(
        current_timestamp - base_timestamp);
}

std::int64_t
RtpNtpTimeMapper::RtpDeltaToMicroseconds(
    std::int32_t rtp_delta,
    std::uint32_t clock_rate) noexcept
{
    if (clock_rate == 0) {
        return 0;
    }

    /*
     * int32_t最大绝对值乘以1000000仍在int64_t范围内，
     * 因此这里不会产生有符号64位溢出。
     */
    return static_cast<std::int64_t>(rtp_delta) *
           1'000'000LL /
           static_cast<std::int64_t>(clock_rate);
}

} // namespace TRANSPORT