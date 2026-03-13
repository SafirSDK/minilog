#include "udp_server.hpp"

#include "parser/syslog_parser.hpp"

namespace minilog
{

UdpServer::UdpServer(boost::asio::io_context& ioc,
                     const Config& cfg,
                     OutputManager& output_mgr,
                     Forwarder* forwarder)
    : m_cfg(cfg), m_socket(ioc), m_output_mgr(output_mgr), m_forwarder(forwarder),
      m_recv_buffer(BUFFER_SIZE)
{
}

void UdpServer::start()
{
    // TODO: open socket, bind to m_cfg.host:m_cfg.udp_port, call receive()
}

void UdpServer::stop()
{
    boost::system::error_code ec;
    m_socket.close(ec);
}

void UdpServer::receive()
{
    m_socket.async_receive_from(boost::asio::buffer(m_recv_buffer),
                                m_sender_endpoint,
                                [this](const boost::system::error_code& ec, std::size_t bytes)
                                { on_receive(ec, bytes); });
}

void UdpServer::on_receive(const boost::system::error_code& ec, std::size_t bytes)
{
    if (ec)
    {
        return;
    }

    // Copy received data immediately so buffer can be re-armed
    std::string data(m_recv_buffer.data(), bytes);
    std::string src_ip = m_sender_endpoint.address().to_string();

    // Re-arm receive before processing
    receive();

    // TODO: decode encoding, parse, dispatch
    (void)data;
    (void)src_ip;
}

} // namespace minilog
