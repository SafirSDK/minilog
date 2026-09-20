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
        // report of an error, which is the one that has nothing to summarise.
        uint64_t suppressed = 0;
    };

    // Record a receive error and say what to do about it.
    //
    // A different message is always reported at once — a new failure is news
    // even in the middle of a streak of another one — and restarts the delay,
    // because it may well be the transient kind. An identical one is counted
    // and summarised at most once per kReportInterval.
    Decision onError(const std::string& message, std::chrono::steady_clock::time_point now)
    {
        Decision decision;

        if (message != m_message)
        {
            m_message       = message;
            m_streak        = 1;
            m_unreported    = 0;
            m_lastReport    = now;
            m_delay         = kFirstDelay;
            decision.delay  = m_delay;
            decision.report = true;
            return decision;
        }

        ++m_streak;
        ++m_unreported;
        m_delay        = std::min(m_delay * 2, kMaxDelay);
        decision.delay = m_delay;

        if (now - m_lastReport >= kReportInterval)
        {
            decision.report     = true;
            decision.suppressed = m_unreported;
            m_unreported        = 0;
            m_lastReport        = now;
        }
        return decision;
    }

    // Record a successful receive, ending any streak. Returns how many
    // consecutive errors it ended, or 0 if there was no streak — so the caller
    // only reports a recovery that somebody was told about in the first place.
    uint64_t onSuccess()
    {
        const uint64_t ended = m_streak;
        m_message.clear();
        m_streak     = 0;
        m_unreported = 0;
        m_delay      = std::chrono::milliseconds{0};
        return ended;
    }

    // The delay the next identical error would wait. For tests.
    [[nodiscard]] std::chrono::milliseconds delay() const { return m_delay; }

    // Short enough that a transient error costs nothing noticeable, long enough
    // that a persistent one cannot spin a core. The cap also bounds how long
    // after an error clears the socket stays un-armed.
    static constexpr std::chrono::milliseconds kFirstDelay{50};
    static constexpr std::chrono::milliseconds kMaxDelay{1000};
    static constexpr std::chrono::seconds kReportInterval{60};

private:
    std::string m_message;
    uint64_t m_streak                 = 0;
    uint64_t m_unreported             = 0;
    std::chrono::milliseconds m_delay = std::chrono::milliseconds{0};
    std::chrono::steady_clock::time_point m_lastReport{};
};

} // namespace minilog
