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

namespace minilog
{

Forwarder::Forwarder(boost::asio::io_context& ioc, const ForwardingConfig& cfg)
    : m_cfg(cfg), m_strand(boost::asio::make_strand(ioc))
{
}

void Forwarder::forward(const SyslogMessage& msg)
{
    boost::asio::post(m_strand, [this, msg]() { doForward(msg); });
}

void Forwarder::doForward(const SyslogMessage& /*msg*/)
{
    // TODO: truncate if needed, send UDP datagram
}

} // namespace minilog
