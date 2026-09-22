/******************************************************************************
 *
 * Copyright Saab AB, 2026 (https://github.com/SafirSDK/minilog)
 *
 * Created by: Lars Hagström / lars@foldspace.nu
 *
 *******************************************************************************
 *
 * This file is part of minilog.
 *
 * minilog is released under the MIT License. See the LICENSE file in
 * the project root for full license information.
 *
 ******************************************************************************/

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace minilog
{

// Paces re-arming the receive socket after an error, and how often the error is
// reported.
//
// Any error other than operation_aborted / bad_descriptor used to be logged and
// re-armed immediately. For a transient error that is right. For a persistent
// one it is a tight loop: one core at 100%, and one osLogError per iteration
// into the host's system log — which for a syslog collector is frequently
// relayed back into minilog, so the loop feeds itself, bounded only by the
// error clearing on its own.
//
// The delay is monotonic: it grows on every error and only a successful receive
// resets it. The error itself is deliberately not an input — neither its message
// nor whether it changed. An earlier draft restarted the backoff whenever the
// message changed, and two errors alternating defeated it completely (#39): the
// socket re-armed every 50 ms and a line went to the system log twenty times a
// second, indefinitely. A changing error is still a failing socket.
//
// Reporting is rate-limited the same way. The first error is reported at once —
// that is the one nobody has been told about. Everything after it is counted
// and summarised at most once per kReportInterval, and the interval spans
// successful receives: resetting it on a success let a socket alternating
// error, success, error, success write two lines per iteration, which is the
// same self-feeding loop at whatever rate datagrams arrive. Recovery is reported
// under the same rule — only a streak somebody was told about gets a "receiving
// again" line.
//
// Separated from UdpServer so the policy can be tested without provoking a real
// socket error, the same way AdmissionControl is. All state belongs to the
// socket strand; nothing here synchronises.
class ReceiveBackoff
{
public:
    struct Decision
    {
        // How long to wait before re-arming the socket.
        std::chrono::milliseconds delay{0};
        // Whether to write a log line for this error.
        bool report = false;
        // Errors folded into this one since the last report, this one included.
        // Zero on the very first report, which has nothing to summarise.
        uint64_t suppressed = 0;
    };

    // Record a receive error and say what to do about it.
    Decision onError(std::chrono::steady_clock::time_point now)
    {
        Decision decision;

        ++m_streak;
        ++m_unreported;

        m_delay        = (m_streak == 1) ? kFirstDelay : std::min(m_delay * 2, kMaxDelay);
        decision.delay = m_delay;

        if (!m_lastReport.has_value() || now - *m_lastReport >= kReportInterval)
        {
            decision.report = true;
            // Nothing to summarise on the very first report: it names the only
            // error there has been.
            if (m_lastReport.has_value())
            {
                decision.suppressed = m_unreported;
            }
            m_unreported     = 0;
            m_lastReport     = now;
            m_streakReported = true;
        }
        return decision;
    }

    // Record a successful receive, ending any streak. Returns how many
    // consecutive errors it ended, or 0 if the streak was never reported — so
    // the caller only announces recovery from an outage somebody was told about.
    // Reporting one that was silent would put back the line per iteration the
    // rate limit removes, on a socket flapping between error and success.
    //
    // The delay resets here and the reporting state does not: re-arming at once
    // is right for a socket that just delivered a datagram, while the log rate
    // has to hold across the success. m_unreported therefore survives, and the
    // occurrences it counts are summarised by whichever later error reaches the
    // interval.
    uint64_t onSuccess()
    {
        const uint64_t ended = m_streakReported ? m_streak : 0;
        m_streak             = 0;
        m_streakReported     = false;
        m_delay              = std::chrono::milliseconds{0};
        return ended;
    }

    // The delay the next error would wait. For tests.
    [[nodiscard]] std::chrono::milliseconds delay() const { return m_delay; }

    // Short enough that a transient error costs nothing noticeable, long enough
    // that a persistent one cannot spin a core. The cap also bounds how long
    // after an error clears the socket stays un-armed.
    static constexpr std::chrono::milliseconds kFirstDelay{50};
    static constexpr std::chrono::milliseconds kMaxDelay{1000};
    static constexpr std::chrono::seconds kReportInterval{60};

private:
    uint64_t m_streak     = 0;
    uint64_t m_unreported = 0;
    // Whether the current streak has produced a log line. Only such a streak
    // gets a recovery line when it ends.
    bool m_streakReported             = false;
    std::chrono::milliseconds m_delay = std::chrono::milliseconds{0};
    // When the last line was written, or unset if none ever was. Unset rather
    // than a default-constructed time_point, so that "no report yet" does not
    // depend on steady_clock's epoch being far enough in the past for the
    // interval to have elapsed — on a host up for less than a minute it has not.
    std::optional<std::chrono::steady_clock::time_point> m_lastReport;
};

} // namespace minilog
