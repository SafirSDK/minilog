#pragma once
#include "config/config.hpp"
#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include <memory>

namespace minilog
{

// Async UDP receiver. Receives datagrams, decodes, parses, and dispatches
// to OutputManager and Forwarder. Uses the shared io_context thread pool.
class UdpServer
{
public:
    UdpServer(boost::asio::io_context& ioc,
              const Config& cfg,
              OutputManager& output_mgr,
              Forwarder* forwarder); // forwarder may be nullptr if disabled

    void start();
    void stop();

private:
    void receive();
    void on_receive(const boost::system::error_code& ec, std::size_t bytes);

    const Config& m_cfg;
    boost::asio::ip::udp::socket m_socket;
    boost::asio::ip::udp::endpoint m_sender_endpoint;
    OutputManager& m_output_mgr;
    Forwarder* m_forwarder;

    static constexpr std::size_t BUFFER_SIZE = 65507;
    std::vector<char> m_recv_buffer;
};

} // namespace minilog
