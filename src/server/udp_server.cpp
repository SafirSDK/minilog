#include "udp_server.hpp"

#include "parser/syslog_parser.hpp"

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
    // TODO: open socket, bind to m_cfg.host:m_cfg.udpPort, call receive()
}

void UdpServer::stop()
{
    boost::system::error_code ec;
    m_socket.close(ec);
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

    // Re-arm receive before processing
    receive();

    // TODO: decode encoding, parse, dispatch
    (void)data;
    (void)srcIp;
}

} // namespace minilog
