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
#include <string>

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
// resets it. It deliberately does not care whether the error message changed.
// It used to, and that made two errors alternating defeat the whole class —
// every call took the "new message" branch, reset the delay to kFirstDelay and
// reported, so the socket re-armed every 50 ms and a line went to the system log
// twenty times a second, indefinitely. Two distinct causes interleaving on one
// socket is enough to produce that: ENOBUFS under load and a per-datagram
// ECONNREFUSED left by an earlier ICMP port-unreachable, say. A changing error
// is still a failing socket, so the re-arm rate should keep falling either way.
//
// Reporting is separate, and rate-limited the same way regardless of message or
// of what happened in between. The first error is reported at once — that is the
// one nobody has been told about. Everything after it is counted and summarised
// at most once per kReportInterval, which is what bounds the log rate no matter
// how the errors vary. A summary says whether the occurrences it covers were all
// the same error, because "still failing" would otherwise read as a claim about
// a single cause that may not hold.
//
// The interval spans successful receives, which is the only thing that bounds
// the log rate for a socket that fails and recovers by turns. An earlier draft
// reset the reporting state on every success, on the reasoning that an error
// after a healthy receive is news: it made a socket alternating error, success,
// error, success write two lines per iteration — the error and the recovery —
// and the delay never left kFirstDelay, since a success resets that too. That is
// the same self-feeding loop #39 removed, at whatever rate datagrams arrive.
// Recovery is reported under the same rule, for the same reason: only a streak
// somebody was told about gets a "receiving again" line, or the recovery becomes
// the line per iteration that the error no longer is. A summary that spans a
// success says so, since the one thing an operator loses by the interval holding
// across successes is any line saying receives got through in between.
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
        // Whether those folded occurrences were all the same error as the one
        // being reported. Only meaningful when suppressed is non-zero.
        bool varied = false;
        // Whether any receive succeeded between the last report and this one.
        // Also only meaningful when suppressed is non-zero — it is what keeps
        // the line from calling a socket that is delivering datagrams between
        // failures one that has not received anything since.
        bool intermittent = false;
    };

    // Record a receive error and say what to do about it.
    //
    // The message is used only to tell the operator whether a summary covers one
    // error or several; it has no say in the delay or in whether to report,
    // because a socket failing in two ways alternately is not a reason to log
    // faster or re-arm sooner than one failing in a single way.
    Decision onError(const std::string& message, std::chrono::steady_clock::time_point now)
    {
        Decision decision;

        // Only occurrences waiting to be summarised can vary. m_unreported, not
        // the streak: it is exactly the set of errors this report will cover, so
        // an error differing from one already reported and from nothing else does
        // not make the next summary a mixed one.
        if (m_unreported != 0 && message != m_message)
        {
            m_varied = true;
        }
        m_message = message;
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
                decision.suppressed   = m_unreported;
                decision.varied       = m_varied;
                decision.intermittent = m_successSinceReport;
            }
            m_unreported         = 0;
            m_varied             = false;
            m_successSinceReport = false;
            m_lastReport         = now;
            m_streakReported     = true;
        }
        return decision;
    }

    // Record a successful receive, ending any streak. Returns how many
    // consecutive errors it ended, or 0 if the streak was never reported — so
    // the caller only announces recovery from an outage somebody was told about.
    // Reporting one that was silent would put back the line per iteration the
    // rate limit removes, on a socket flapping between error and success.
    //
    // The count spans the whole streak whatever the errors in it were, which is
    // what keeps "receiving again after N consecutive receive error(s)" true of
    // a mixed one.
    //
    // The delay resets here and the reporting state does not: re-arming at once
    // is right for a socket that just delivered a datagram, while the log rate
    // has to hold across the success. m_unreported, m_varied and m_message
    // therefore survive, and the occurrences they count are summarised by
    // whichever later error reaches the interval.
    uint64_t onSuccess()
    {
        const uint64_t ended = m_streakReported ? m_streak : 0;
        m_streak             = 0;
        m_streakReported     = false;
        m_successSinceReport = true;
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
    // The most recent error, kept only to notice that it changed.
    std::string m_message;
    uint64_t m_streak     = 0;
    uint64_t m_unreported = 0;
    bool m_varied         = false;
    // Whether the current streak has produced a log line. Only such a streak
    // gets a recovery line when it ends.
    bool m_streakReported = false;
    // Whether a receive has succeeded since the last line was written. Reported
    // rather than merely counted, because a summary that spans a success is
    // describing a socket failing by turns, not one that has stopped receiving.
    bool m_successSinceReport         = false;
    std::chrono::milliseconds m_delay = std::chrono::milliseconds{0};
    // When the last line was written, or unset if none ever was. Unset rather
    // than a default-constructed time_point, so that "no report yet" does not
    // depend on steady_clock's epoch being far enough in the past for the
    // interval to have elapsed — on a host up for less than a minute it has not.
    std::optional<std::chrono::steady_clock::time_point> m_lastReport;
};

} // namespace minilog
