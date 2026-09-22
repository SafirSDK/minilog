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

#include <chrono>
#include <cstdint>

namespace minilog
{

// Decides how often a sink closed by a filesystem error is reported while it is
// closed, and carries the interval at which it tries to open its files again.
//
// #13 made a filesystem error take one sink out of service instead of stopping
// the process, and left it out for the life of the process. The triggers it
// lists are mostly transient — a network path that blips, a backup agent holding
// a handle for a few seconds, a brief permissions change while ACLs are being
// provisioned — so a two-second fault cost a whole facility's log until somebody
// restarted the service, and the only trace was one line written at the moment
// it happened. Anyone looking later saw a healthy service and a log file that
// stopped mid-afternoon.
//
// The retry is on a timer rather than on the next write to reach the sink. A
// sink is selected by facility, so the one whose silence is least likely to be
// noticed is exactly the quiet one that no write would ever wake.
//
// The interval is not configurable. #31 and #32 spent this release removing
// config surface rather than adding it, and no deployment has a reason to prefer
// a different number: it is short enough that a blip costs seconds of one sink
// and long enough that a permanent fault is not reopening files in a loop.
//
// Reporting is rate-limited exactly as ReceiveBackoff's is, and for the same
// reason: one line per retry for as long as a permanent fault lasts goes into
// the host's system log, which on a collector is frequently relayed back into
// minilog. The failure that closes the sink is reported at once — it is the one
// nobody has been told about — and every failed reopen after it is counted and
// summarised at most once per kReportInterval. That is what makes the loss
// ongoing rather than a one-off without making it a flood.
//
// Separated from LogFile so the policy can be tested without provoking a real
// filesystem error, the same way ReceiveBackoff and AdmissionControl are. All
// state belongs to the sink's strand; nothing here synchronises.
class SinkRecovery
{
public:
    struct Decision
    {
        // Whether to write a log line for this failure.
        bool report = false;
        // Whether this failure is the one that closed a sink that was open. The
        // line differs: that one names what failed and says the sink is closing,
        // where a later one reports that a closed sink is still closed.
        bool firstFailure = false;
        // Failed reopen attempts folded into this report, this one included.
        // Zero on the first failure, which has nothing to summarise.
        uint64_t suppressed = 0;
        // How long the sink has been closed. Zero for the first failure.
        std::chrono::seconds closedFor{0};
    };

    // Record a failure — either the one closing the sink or a reopen attempt
    // that failed — and say whether to report it.
    Decision onFailure(std::chrono::steady_clock::time_point now)
    {
        Decision decision;

        if (m_failures == 0)
        {
            // The failure that closes the sink. Always reported, and it starts
            // the clock the summaries and the recovery line measure against.
            m_closedSince         = now;
            m_lastReport          = now;
            m_failures            = 1;
            decision.report       = true;
            decision.firstFailure = true;
            return decision;
        }

        ++m_failures;
        ++m_unreported;

        if (now - m_lastReport >= kReportInterval)
        {
            decision.report     = true;
            decision.suppressed = m_unreported;
            decision.closedFor =
                std::chrono::duration_cast<std::chrono::seconds>(now - m_closedSince);
            m_unreported = 0;
            m_lastReport = now;
        }
        return decision;
    }

    struct Outage
    {
        // How long the sink was closed, and how many failures it took — the one
        // that closed it included, so this is one more than the number of failed
        // reopen attempts. Both are reported: an outage somebody is reading
        // about a day later needs a duration to be a fault rather than a blip.
        std::chrono::seconds closedFor{0};
        uint64_t failures = 0;
    };

    // Record that the files opened again, and describe the outage that ended.
    // Every outage was reported when it began, so every recovery gets a line;
    // there is no silent-streak case to suppress the way ReceiveBackoff has.
    //
    // This resets the reporting state, so a fault that clears and returns inside
    // one report interval is reported afresh rather than suppressed. That is
    // deliberate: each recovery genuinely ends an outage, and the pair of lines
    // is what tells an operator the storage is flapping rather than simply down.
    // The retry interval bounds how often that can happen.
    // Returns a zeroed Outage if the sink was not closed, which LogFile never
    // does — the call sites are the retry path only.
    Outage onRecovered(std::chrono::steady_clock::time_point now)
    {
        Outage outage;
        if (m_failures == 0)
        {
            return outage;
        }
        outage.closedFor = std::chrono::duration_cast<std::chrono::seconds>(now - m_closedSince);
        outage.failures  = m_failures;

        m_failures   = 0;
        m_unreported = 0;
        return outage;
    }

    // Whether a sink is currently counted as closed. For tests.
    [[nodiscard]] bool closed() const { return m_failures != 0; }

    // How long a closed sink waits before trying to open its files again.
    static constexpr std::chrono::seconds kRetryInterval{30};

    // How often a sink that is still closed is reported. The same interval as
    // ReceiveBackoff's, for the same reason.
    static constexpr std::chrono::seconds kReportInterval{60};

private:
    // Failures in the current outage, the one that closed the sink included.
    // Zero means the sink is open, which is what makes the first failure
    // distinguishable from a failed reopen without a separate flag.
    uint64_t m_failures = 0;
    // Failures waiting to be summarised.
    uint64_t m_unreported = 0;
    // Both are only read while m_failures is non-zero, and both are set before
    // it becomes so — so neither needs to mean anything for an open sink.
    std::chrono::steady_clock::time_point m_closedSince;
    std::chrono::steady_clock::time_point m_lastReport;
};

} // namespace minilog
