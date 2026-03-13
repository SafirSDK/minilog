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
#include <boost/asio/strand.hpp>

#include <optional>
#include <string>

namespace minilog
{

// Forwards messages to a remote syslog server over UDP.
// Messages exceeding max_message_size are truncated before sending.
class Forwarder
{
public:
    Forwarder(boost::asio::io_context& ioc, const ForwardingConfig& cfg);

    // Dispatch a forward to this forwarder's strand (non-blocking for caller).
    void forward(const SyslogMessage& msg);

private:
    void doForward(const SyslogMessage& msg);

    static bool facilityMatches(const std::vector<int>& filter,
                                const std::optional<int>& msgFacility);
    static std::string truncateIfNeeded(const std::string& raw, uint32_t maxSize);

    ForwardingConfig m_cfg;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;
    boost::asio::ip::udp::socket m_socket;
    boost::asio::ip::udp::endpoint m_endpoint;
};

} // namespace minilog
