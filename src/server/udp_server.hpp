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
              OutputManager& outputMgr,
              Forwarder* forwarder); // forwarder may be nullptr if disabled

    void start();
    void stop();

    // Returns the actual bound port (useful when udpPort=0 was requested).
    [[nodiscard]] uint16_t localPort() const;

private:
    void receive();
    void onReceive(const boost::system::error_code& ec, std::size_t bytes);

    const Config& m_cfg;
    boost::asio::ip::udp::socket m_socket;
    boost::asio::ip::udp::endpoint m_senderEndpoint;
    OutputManager& m_outputMgr;
    Forwarder* m_forwarder;

    static constexpr std::size_t BUFFER_SIZE = 65507;
    std::vector<char> m_recvBuffer;
};

} // namespace minilog
