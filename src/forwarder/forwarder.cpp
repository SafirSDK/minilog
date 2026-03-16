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

namespace minilog
{

Forwarder::Forwarder(boost::asio::io_context& ioc, ForwardingConfig cfg)
    : m_cfg(std::move(cfg)), m_strand(boost::asio::make_strand(ioc)), m_socket(ioc)
{
    if (m_cfg.enabled)
    {
        m_socket.open(boost::asio::ip::udp::v4());
        m_endpoint =
            boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(m_cfg.host), m_cfg.port);
    }
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
