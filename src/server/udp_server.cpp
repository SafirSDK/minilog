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

        // Request a large kernel receive buffer to absorb syslog bursts.
        // The OS silently caps this at net.core.rmem_max, so no error check.
        m_socket.set_option(boost::asio::socket_base::receive_buffer_size(4 * 1024 * 1024));

#ifdef _WIN32
        // Windows allows UDP port-sharing by default; SO_EXCLUSIVEADDRUSE
        // prevents any other process from binding the same port.
        const BOOL exclusive = TRUE;
        setsockopt(m_socket.native_handle(),
                   SOL_SOCKET,
                   SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive));
#endif
        m_socket.bind(ep);
    }
    catch (const boost::system::system_error& e)
    {
        const std::string msg =
            "minilog: failed to bind UDP port " + std::to_string(m_cfg.udpPort) + ": " + e.what();
        osLogError(msg);
        throw std::runtime_error(msg);
    }

    receive();
}

void UdpServer::stop()
{
    boost::asio::post(m_socket.get_executor(),
                      [this]()
                      {
                          boost::system::error_code ec;
                          m_socket.close(
                              ec); // NOLINT(bugprone-unused-return-value) — close(ec) returns void
                      });
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
        // operation_aborted: socket cancelled by stop().
        // bad_descriptor: receive() was called on a socket that was already
        // closed (race between the re-arm in the success path and stop()).
        // Either way the socket is gone — do not re-arm.
        if (ec != boost::asio::error::operation_aborted &&
            ec != boost::asio::error::bad_descriptor)
        {
            osLogError("minilog: receive error: " + ec.message());
            receive();
        }
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
