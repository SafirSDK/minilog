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

#include "forwarder.hpp"

#include "platform/os_log.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>

namespace minilog
{

namespace
{

// The host as written, quoted. The port is deliberately left out: it plays no
// part in resolution, and "'syslog.example.com:514'" would read like the
// port-appended value that loadConfig rejects.
std::string describe(const ForwardingConfig& cfg)
{
    return "'" + cfg.host + "'";
}

} // namespace

Forwarder::Forwarder(boost::asio::io_context& ioc, ForwardingConfig cfg)
    : m_cfg(std::move(cfg)), m_strand(boost::asio::make_strand(ioc)), m_socket(m_strand),
      m_resolver(m_strand), m_retryTimer(m_strand)
{
    if (!m_cfg.enabled)
    {
        return;
    }

    // Started here, finished on the io_context. Resolving synchronously in the
    // constructor made a reachable destination usable from the first datagram,
    // but it did so by blocking in getaddrinfo before the UDP socket binds and,
    // on Windows, before the SCM is told the service is running. An IP literal
    // takes this path too — the resolver handles both, which is what removes
    // the need to decide which one was written — and for a literal it finishes
    // essentially at once.
    startResolve(/*firstAttempt=*/true);
}

void Forwarder::startResolve(bool firstAttempt)
{
    m_resolver.async_resolve(
        m_cfg.host,
        std::to_string(m_cfg.port),
        [this, firstAttempt](const boost::system::error_code& ec,
                             const boost::asio::ip::udp::resolver::results_type& results)
        {
            ++m_resolveAttempts;
            if (m_stopping)
            {
                return;
            }
            if (!ec && !results.empty())
            {
                useEndpoint(*results.begin());
                return;
            }
            if (firstAttempt)
            {
                // Reported once, on the first failure only. The line names the
                // host and says retries will continue, so repeating it every
                // minute would fill the Event Log with one fact.
                m_failureReported = true;
                osLogError("minilog: cannot resolve [forwarding] host " + describe(m_cfg) + ": " +
                           (ec ? ec.message() : std::string("no addresses returned")) +
                           ". Forwarding is off and resolution will be retried in the background; "
                           "the rest of minilog is unaffected.");
            }
            else
            {
                m_retryDelay = std::min(m_retryDelay * 2, kMaxRetryDelay);
            }
            scheduleResolveRetry();
        });
}

void Forwarder::useEndpoint(const boost::asio::ip::udp::endpoint& endpoint)
{
    // The protocol comes from the endpoint rather than being assumed to be
    // IPv4, which is what makes an IPv6 destination work at all.
    //
    // Opened with an error_code because this now runs on the io_context rather
    // than in the constructor: a throw here would escape into runIoContext and
    // be reported as an unhandled handler exception, which says nothing about
    // forwarding. Retried like a failed lookup, since whatever exhausted the
    // descriptors may not still be doing so.
    boost::system::error_code ec;
    m_socket.open(endpoint.protocol(), ec);
    if (ec)
    {
        if (!m_failureReported)
        {
            m_failureReported = true;
            osLogError("minilog: cannot open a forwarding socket for [forwarding] host " +
                       describe(m_cfg) + ": " + ec.message() +
                       ". Forwarding is off and will be retried in the background; the rest of "
                       "minilog is unaffected.");
        }
        m_retryDelay = std::min(m_retryDelay * 2, kMaxRetryDelay);
        scheduleResolveRetry();
        return;
    }
    m_endpoint = endpoint;
    m_resolved = true;

    if (m_droppedUnresolved != 0)
    {
        const std::string where = "minilog: [forwarding] host " + describe(m_cfg) +
                                  " resolved to " + m_endpoint.address().to_string() + " port " +
                                  std::to_string(m_cfg.port) + "; ";
        // Two different events, and saying "resumed" for both would invent an
        // outage that never happened. Nothing was ever wrong in the second
        // case: the startup lookup simply had not finished when the first
        // datagrams arrived, which is the cost of not blocking the bind on it.
        osLogInfo(m_failureReported
                      ? where + "forwarding resumed after dropping " +
                            std::to_string(m_droppedUnresolved) +
                            " message(s) while it was unresolved"
                      : where + "forwarding started; " + std::to_string(m_droppedUnresolved) +
                            " message(s) arrived before the lookup finished and were not "
                            "forwarded");
        m_droppedUnresolved = 0;
    }
}

void Forwarder::scheduleResolveRetry()
{
    if (m_stopping)
    {
        return;
    }
    m_retryTimer.expires_after(m_retryDelay);
    m_retryTimer.async_wait(
        [this](const boost::system::error_code& ec)
        {
            if (ec || m_stopping)
            {
                return; // cancelled at shutdown
            }
            startResolve(/*firstAttempt=*/false);
        });
}

void Forwarder::stop()
{
    boost::asio::post(m_strand,
                      [this]()
                      {
                          m_stopping = true;
                          m_retryTimer.cancel();
                          m_resolver.cancel();
                      });
}

void Forwarder::forward(const SyslogMessage& msg)
{
    boost::asio::post(m_strand, [this, msg]() { doForward(msg); });
}

void Forwarder::doForward(const SyslogMessage& msg)
{
    if (!m_cfg.enabled)
    {
        return;
    }
    if (!facilityMatches(m_cfg.facilities, msg.facility))
    {
        return;
    }
    if (!m_resolved)
    {
        // Counted rather than reported: the destination being unresolved is
        // already in the log, and a message per datagram would bury it.
        ++m_droppedUnresolved;
        return;
    }
    const std::string payload = truncateIfNeeded(msg.raw, m_cfg.maxMessageSize);
    boost::system::error_code ec;
    m_socket.send_to(boost::asio::buffer(payload), m_endpoint, 0, ec);
    if (ec)
    {
        osLogError("minilog: forward to " + m_cfg.host + ":" + std::to_string(m_cfg.port) +
                   " failed: " + ec.message());
    }
}

std::string Forwarder::truncateIfNeeded(const std::string& raw, uint32_t maxSize)
{
    if (maxSize == 0 || raw.size() <= maxSize)
    {
        return raw;
    }
    const std::string suffix = "[TRUNCATED: " + std::to_string(raw.size()) + " bytes]";
    if (suffix.size() >= maxSize)
    {
        return raw.substr(0, maxSize);
    }
    return raw.substr(0, maxSize - suffix.size()) + suffix;
}

} // namespace minilog
