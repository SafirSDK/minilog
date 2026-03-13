#pragma once
#include "config/config.hpp"
#include "output/output_manager.hpp"
#include "forwarder/forwarder.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <memory>

namespace minilog {

// Async UDP receiver. Receives datagrams, decodes, parses, and dispatches
// to OutputManager and Forwarder. Uses the shared io_context thread pool.
class UdpServer {
public:
    UdpServer(boost::asio::io_context& ioc,
              const Config& cfg,
              OutputManager& output_mgr,
              Forwarder* forwarder);  // forwarder may be nullptr if disabled

    void start();
    void stop();

private:
    void receive();
    void on_receive(const boost::system::error_code& ec, std::size_t bytes);

    const Config&                       cfg_;
    boost::asio::ip::udp::socket        socket_;
    boost::asio::ip::udp::endpoint      sender_endpoint_;
    OutputManager&                      output_mgr_;
    Forwarder*                          forwarder_;

    static constexpr std::size_t BUFFER_SIZE = 65507;
    std::vector<char>                   recv_buffer_;
};

} // namespace minilog
