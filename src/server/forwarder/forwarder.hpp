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
#include "config/config.hpp"
#include "parser/syslog_message.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace minilog
{

// Forwards messages to a remote syslog server over UDP.
// Messages exceeding max_message_size are truncated before sending.
//
// The destination may be a hostname or an IP literal. It is resolved once, at
// construction, and the endpoint is then used for the lifetime of the process:
// re-resolving per message would put a name lookup on the hot path, and a timer
// re-resolving periodically buys nothing until a deployment turns up whose
// collector actually moves.
//
// The lookup is *started* by the constructor and completes on the io_context.
// It used to be a blocking getaddrinfo in the constructor, which on Windows ran
// inside the window the SCM times a start in, and on every platform delayed the
// UDP bind that follows it. An unresponsive resolver is exactly when that call
// blocks longest — tens of seconds — and is also the case the retry below
// exists for, so the two combined could have the SCM declare a start hung. The
// cost of the asynchronous form is that datagrams arriving before the lookup
// returns are dropped; that is a state the retry path already concedes on every
// failure, and they are counted and reported like any other.
//
// A name that does not resolve at startup is not a startup failure. A Windows
// AUTO_START service is routinely running before DNS is, so treating "not yet"
// as a fatal config error would make boot ordering a hazard. Instead the failure
// is reported once, forwarding stays off, and resolution is retried on the
// strand with a growing delay until it succeeds. Everything else about minilog
// keeps working in the meantime, and the messages that could not be forwarded
// are counted so the eventual success can say how many were lost.
class Forwarder
{
public:
    Forwarder(boost::asio::io_context& ioc, ForwardingConfig cfg);

    // Dispatch a forward to this forwarder's strand (non-blocking for caller).
    void forward(const SyslogMessage& msg);

    // Cancel a pending resolution retry so the io_context can run out of work.
    // Safe to call from any thread, and safe to call when nothing is pending.
    //
    // It does not cancel a lookup that is already in flight. Asio runs
    // getaddrinfo on a thread of its own and resolver::cancel() only reaches
    // operations still queued, so a stop that lands during a lookup waits for
    // that lookup to return — the resolver's own timeout, at worst. There is no
    // timeout parameter to shorten it with, and taking the lookup off the
    // io_context entirely means a detached thread outliving this object. The
    // delay is accepted and documented rather than engineered around.
    void stop();

    // Whether the destination has been resolved and the socket opened. For
    // tests: read it only while no thread is inside io_context::run(), since it
    // is written on the strand.
    [[nodiscard]] bool resolved() const { return m_resolved; }

    // How many lookups have come back, successfully or not. For tests, under the
    // same rule as resolved(). It is what lets a test wait for a failed lookup —
    // which has no other outward sign, since the failure is reported to the
    // system log and forwarding simply stays off.
    [[nodiscard]] uint64_t resolveAttempts() const { return m_resolveAttempts; }

private:
    void doForward(const SyslogMessage& msg);

    // Adopt a resolved endpoint: open the socket with that endpoint's protocol,
    // which is what lets an IPv6 destination work, and report recovery.
    void useEndpoint(const boost::asio::ip::udp::endpoint& endpoint);

    // Start an asynchronous lookup of the destination. firstAttempt marks the
    // one the constructor starts: only it reports a failure, and only a retry
    // grows the delay.
    void startResolve(bool firstAttempt);

    void scheduleResolveRetry();

    static std::string truncateIfNeeded(const std::string& raw, uint32_t maxSize);

    // Retry delay for an unresolved destination: doubles from the first value to
    // the cap. Short at first because DNS coming up a moment after the service
    // does is the case this exists for; capped so a permanently wrong name costs
    // nothing to keep retrying.
    static constexpr std::chrono::seconds kFirstRetryDelay{1};
    static constexpr std::chrono::seconds kMaxRetryDelay{60};

    ForwardingConfig m_cfg;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;
    boost::asio::ip::udp::socket m_socket;
    boost::asio::ip::udp::endpoint m_endpoint;
    boost::asio::ip::udp::resolver m_resolver;
    boost::asio::steady_timer m_retryTimer;
    std::chrono::seconds m_retryDelay = kFirstRetryDelay;
    uint64_t m_droppedUnresolved      = 0;
    uint64_t m_resolveAttempts        = 0;
    bool m_resolved                   = false;
    bool m_stopping                   = false;
    // Whether a failure was ever reported for this destination. It decides how
    // success reads: recovering from a reported outage is not the same event as
    // the first lookup simply finishing after a few datagrams had arrived.
    bool m_failureReported = false;
};

} // namespace minilog
