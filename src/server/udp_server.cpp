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

#include "udp_server.hpp"

#include "parser/syslog_parser.hpp"
#include "platform/os_log.hpp"

#include <boost/asio/post.hpp>

#include <stdexcept>

namespace minilog
{

UdpServer::UdpServer(boost::asio::io_context& ioc,
                     const Config& cfg,
                     OutputManager& outputMgr,
                     Forwarder* forwarder)
    : m_cfg(cfg), m_socket(ioc), m_outputMgr(outputMgr), m_forwarder(forwarder),
      m_recvBuffer(BUFFER_SIZE)
{
}

void UdpServer::start()
{
    using udp          = boost::asio::ip::udp;
    const auto address = boost::asio::ip::make_address(m_cfg.host);
    const udp::endpoint ep(address, m_cfg.udpPort);

    try
    {
        m_socket.open(ep.protocol());
        m_socket.bind(ep);
    }
    catch (const boost::system::system_error& e)
    {
        const std::string msg =
            "minilog: failed to bind UDP port " + std::to_string(m_cfg.udpPort) + ": " + e.what();
        os_log_error(msg);
        throw std::runtime_error(msg);
    }

    receive();
}

void UdpServer::stop()
{
    boost::system::error_code ec;
    m_socket.close(ec);
}

uint16_t UdpServer::localPort() const
{
    return m_socket.local_endpoint().port();
}

void UdpServer::receive()
{
    m_socket.async_receive_from(boost::asio::buffer(m_recvBuffer),
                                m_senderEndpoint,
                                [this](const boost::system::error_code& ec, std::size_t bytes)
                                { onReceive(ec, bytes); });
}

void UdpServer::onReceive(const boost::system::error_code& ec, std::size_t bytes)
{
    if (ec)
    {
        return;
    }

    // Copy received data immediately so buffer can be re-armed
    std::string data(m_recvBuffer.data(), bytes);
    std::string srcIp = m_senderEndpoint.address().to_string();

    // Re-arm immediately so the next datagram isn't missed.
    receive();

    // Post the parse + dispatch work so it can run on any io_context thread.
    boost::asio::post(m_socket.get_executor(),
                      [this, data = std::move(data), srcIp = std::move(srcIp)]()
                      {
                          SyslogMessage msg = parseSyslog(data);
                          msg.srcIp         = srcIp;
                          m_outputMgr.dispatch(msg);
                          if (m_forwarder != nullptr)
                          {
                              m_forwarder->forward(msg);
                          }
                      });
}

} // namespace minilog
