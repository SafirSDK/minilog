#include "udp_server.hpp"
#include "parser/syslog_parser.hpp"

namespace minilog {

UdpServer::UdpServer(boost::asio::io_context& ioc,
                     const Config& cfg,
                     OutputManager& output_mgr,
                     Forwarder* forwarder)
    : cfg_(cfg)
    , socket_(ioc)
    , output_mgr_(output_mgr)
    , forwarder_(forwarder)
    , recv_buffer_(BUFFER_SIZE)
{}

void UdpServer::start()
{
    // TODO: open socket, bind to cfg_.host:cfg_.udp_port, call receive()
}

void UdpServer::stop()
{
    boost::system::error_code ec;
    socket_.close(ec);
}

void UdpServer::receive()
{
    socket_.async_receive_from(
        boost::asio::buffer(recv_buffer_),
        sender_endpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytes) {
            on_receive(ec, bytes);
        }
    );
}

void UdpServer::on_receive(const boost::system::error_code& ec, std::size_t bytes)
{
    if (ec) return;

    // Copy received data immediately so buffer can be re-armed
    std::string data(recv_buffer_.data(), bytes);
    std::string src_ip = sender_endpoint_.address().to_string();

    // Re-arm receive before processing
    receive();

    // TODO: decode encoding, parse, dispatch
    (void)data;
    (void)src_ip;
}

} // namespace minilog
