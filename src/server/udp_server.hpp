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
#include "admission.hpp"
#include "receive_backoff.hpp"

#include "config/config.hpp"
#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <memory>
#include <optional>

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

    // Datagrams refused so far because the receive queue was at its budget.
    [[nodiscard]] uint64_t droppedDatagrams() const { return m_admission.droppedTotal(); }

    // Bytes of accepted-but-unwritten datagrams currently held.
    [[nodiscard]] uint64_t queuedBytes() const { return m_admission.inFlight(); }

private:
    void receive();
    void onReceive(const boost::system::error_code& ec, std::size_t bytes);

    // Logs the drops accumulated since the last report, at most once per
    // DROP_REPORT_INTERVAL unless forced. Silent data loss in a logging product
    // is its own bug, but a line per dropped datagram would be the flood again
    // in the Event Log. Called only from the socket strand.
    void reportDrops(bool force);

    // Report a receive error and re-arm after the backoff the policy asks for,
    // rather than immediately. Called only from the socket strand.
    void handleReceiveError(const boost::system::error_code& ec);

    const Config& m_cfg;
    boost::asio::io_context& m_ioc;
    boost::asio::ip::udp::socket m_socket;
    boost::asio::ip::udp::endpoint m_senderEndpoint;
    OutputManager& m_outputMgr;
    Forwarder* m_forwarder;
    AdmissionControl m_admission;
    ReceiveBackoff m_backoff;

    // Delays the re-arm after a receive error. Cancelled by stop(): an
    // outstanding timer is work, and io_context::run() does not return while
    // there is work outstanding.
    boost::asio::steady_timer m_rearmTimer;

    // Socket-strand only, so no synchronisation. Unset until the first report.
    std::optional<std::chrono::steady_clock::time_point> m_lastDropReport;

    static constexpr std::size_t BUFFER_SIZE = 65507;
    static constexpr std::chrono::seconds DROP_REPORT_INTERVAL{10};
    std::vector<char> m_recvBuffer;
};

} // namespace minilog
