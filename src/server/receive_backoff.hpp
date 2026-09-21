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
// Reporting is separate, and rate-limited the same way regardless of message.
// The first error after a successful receive is always reported at once — that
// is the one nobody has been told about. Everything after it, same message or
// not, is counted and summarised at most once per kReportInterval, which is what
// bounds the log rate no matter how the errors vary. A summary says whether the
// occurrences it covers were all the same error, because "still failing" would
// otherwise read as a claim about a single cause that may not hold.
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
        // Errors folded into this one since the last report. Zero on the first
        // report of a streak, which is the one that has nothing to summarise.
        uint64_t suppressed = 0;
        // Whether those folded occurrences were all the same error as the one
        // being reported. Only meaningful when suppressed is non-zero.
        bool varied = false;
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

        // Only occurrences folded into the same report can vary: the first error
        // of a streak is reported on its own and names itself.
        if (m_streak != 0 && message != m_message)
        {
            m_varied = true;
        }
        m_message = message;
        ++m_streak;

        m_delay        = (m_streak == 1) ? kFirstDelay : std::min(m_delay * 2, kMaxDelay);
        decision.delay = m_delay;

        if (m_streak == 1)
        {
            m_lastReport    = now;
            decision.report = true;
            return decision;
        }

        ++m_unreported;

        if (now - m_lastReport >= kReportInterval)
        {
            decision.report     = true;
            decision.suppressed = m_unreported;
            decision.varied     = m_varied;
            m_unreported        = 0;
            m_varied            = false;
            m_lastReport        = now;
        }
        return decision;
    }

    // Record a successful receive, ending any streak. Returns how many
    // consecutive errors it ended, or 0 if there was no streak — so the caller
    // only reports a recovery that somebody was told about in the first place.
    //
    // The count spans the whole streak whatever the errors in it were, which is
    // what keeps "receiving again after N consecutive receive error(s)" true of
    // a mixed one.
    uint64_t onSuccess()
    {
        const uint64_t ended = m_streak;
        m_message.clear();
        m_streak     = 0;
        m_unreported = 0;
        m_varied     = false;
        m_delay      = std::chrono::milliseconds{0};
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
    uint64_t m_streak                 = 0;
    uint64_t m_unreported             = 0;
    bool m_varied                     = false;
    std::chrono::milliseconds m_delay = std::chrono::milliseconds{0};
    std::chrono::steady_clock::time_point m_lastReport{};
};

} // namespace minilog
