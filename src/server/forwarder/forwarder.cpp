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

    // Resolved synchronously here, before the io_context runs, so that a
    // destination which is reachable is usable from the first datagram rather
    // than for everything after an asynchronous lookup happens to finish. An IP
    // literal takes this path too — the resolver handles both, which is what
    // removes the need to decide which one was written.
    boost::system::error_code ec;
    const auto results = m_resolver.resolve(m_cfg.host, std::to_string(m_cfg.port), ec);
    if (!ec && !results.empty())
    {
        useEndpoint(*results.begin());
        return;
    }

    osLogError("minilog: cannot resolve [forwarding] host " + describe(m_cfg) + ": " +
               (ec ? ec.message() : std::string("no addresses returned")) +
               ". Forwarding is off and resolution will be retried in the background; the rest of "
               "minilog is unaffected.");
    scheduleResolveRetry();
}

void Forwarder::useEndpoint(const boost::asio::ip::udp::endpoint& endpoint)
{
    // The protocol comes from the endpoint rather than being assumed to be
    // IPv4, which is what makes an IPv6 destination work at all.
    m_socket.open(endpoint.protocol());
    m_endpoint = endpoint;
    m_resolved = true;

    if (m_droppedUnresolved != 0)
    {
        osLogInfo("minilog: [forwarding] host " + describe(m_cfg) + " resolved to " +
                  m_endpoint.address().to_string() + " port " + std::to_string(m_cfg.port) +
                  "; forwarding resumed after dropping " + std::to_string(m_droppedUnresolved) +
                  " message(s) while it was unresolved");
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
            // Asynchronous, unlike the one in the constructor: this runs on a
            // worker thread, and a blocking lookup there would stall ingestion
            // for the resolver's timeout every time it fired.
            m_resolver.async_resolve(
                m_cfg.host,
                std::to_string(m_cfg.port),
                [this](const boost::system::error_code& rec,
                       const boost::asio::ip::udp::resolver::results_type& results)
                {
                    if (m_stopping)
                    {
                        return;
                    }
                    if (!rec && !results.empty())
                    {
                        useEndpoint(*results.begin());
                        return;
                    }
                    // Deliberately not reported again. The first failure named
                    // the host and said retries would continue; repeating it
                    // every minute would fill the Event Log with one fact.
                    m_retryDelay = std::min(m_retryDelay * 2, kMaxRetryDelay);
                    scheduleResolveRetry();
                });
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
