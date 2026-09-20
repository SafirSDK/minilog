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

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace minilog
{

UdpServer::UdpServer(boost::asio::io_context& ioc,
                     const Config& cfg,
                     OutputManager& outputMgr,
                     Forwarder* forwarder)
    : m_cfg(cfg), m_ioc(ioc), m_socket(boost::asio::make_strand(ioc)), m_outputMgr(outputMgr),
      m_forwarder(forwarder), m_admission(cfg.maxQueueBytes), m_rearmTimer(m_socket.get_executor()),
      m_recvBuffer(BUFFER_SIZE)
{
}

void UdpServer::start()
{
    using udp = boost::asio::ip::udp;

    // loadConfig validates this, so a failure here means UdpServer was built from
    // a config that did not come through it. Report it rather than throwing a
    // bare system_error from outside the handler below.
    boost::system::error_code addrEc;
    const auto address = boost::asio::ip::make_address(m_cfg.host, addrEc);
    if (addrEc)
    {
        throw std::runtime_error("minilog: invalid [server] host '" + m_cfg.host +
                                 "': " + addrEc.message());
    }
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
        // Reported by the caller, so a bind failure is not logged twice.
        throw std::runtime_error("minilog: failed to bind UDP port " +
                                 std::to_string(m_cfg.udpPort) + ": " + e.what());
    }

    receive();
}

void UdpServer::stop()
{
    boost::asio::post(m_socket.get_executor(),
                      [this]()
                      {
                          // Close first. It is the load-bearing half of stopping,
                          // and reportDrops builds strings, so it can throw under
                          // exactly the memory pressure it exists to report on. A
                          // throw before the close would leave the socket armed,
                          // and runIoContext re-enters run() after a handler
                          // exception, so shutdown would never finish.
                          boost::system::error_code ec;
                          m_socket.close(
                              ec); // NOLINT(bugprone-unused-return-value) — close(ec) returns void

                          // A pending re-arm is outstanding work, and run() does
                          // not return while there is any. Without this, a
                          // shutdown during a receive-error streak would wait out
                          // the backoff before finishing.
                          m_rearmTimer.cancel();

                          // Whatever is left over, rather than losing the tail of
                          // a flood that stopped before the interval elapsed.
                          reportDrops(true);
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

// A receive error used to log and re-arm straight away, which is correct for a
// transient error and a tight loop for a persistent one — a core at 100% and a
// log line per iteration, relayed straight back into minilog on a host whose
// syslog it collects. The delay and the reporting interval come from
// ReceiveBackoff; the operation_aborted / bad_descriptor handling above is
// deliberately untouched, because those mean the socket is gone.
void UdpServer::handleReceiveError(const boost::system::error_code& ec)
{
    const auto decision = m_backoff.onError(ec.message(), std::chrono::steady_clock::now());

    if (decision.report)
    {
        std::string message = "minilog: receive error: " + ec.message();
        if (decision.suppressed != 0)
        {
            message += " (still failing; " + std::to_string(decision.suppressed) +
                       " further occurrence(s) since the last report)";
        }
        osLogError(message);
    }

    m_rearmTimer.expires_after(decision.delay);
    m_rearmTimer.async_wait(
        [this](const boost::system::error_code& timerEc)
        {
            if (timerEc)
            {
                return; // cancelled by stop()
            }
            receive();
        });
}

void UdpServer::onReceive(const boost::system::error_code& ec, std::size_t bytes)
{
    if (ec)
    {
        // operation_aborted: socket cancelled by stop().
        // bad_descriptor: receive() was called on a socket that was already
        // closed (race between the re-arm in the success path and stop()).
        // Either way the socket is gone — do not re-arm.
        if (ec != boost::asio::error::operation_aborted && ec != boost::asio::error::bad_descriptor)
        {
            handleReceiveError(ec);
        }
        return;
    }

    if (const uint64_t ended = m_backoff.onSuccess(); ended != 0)
    {
        osLogInfo("minilog: receiving again after " + std::to_string(ended) +
                  " consecutive receive error(s)");
    }

    // Admission control, before the first copy: nothing downstream of here
    // applies back pressure, so this is where a sender faster than the disk is
    // refused. The token rides along on the message and releases the charge
    // once the last queued copy of it is gone.
    auto admission = m_admission.admit(bytes);
    if (!admission)
    {
        reportDrops(false);
        receive();
        return;
    }

    // Copy received data immediately so buffer can be re-armed
    std::string data(m_recvBuffer.data(), bytes);
    std::string srcIp = m_senderEndpoint.address().to_string();

    // Re-arm immediately so the next datagram isn't missed.
    receive();

    // Post the parse + dispatch work to the io_context so it can run on any
    // thread in the pool, not serialised on the receive strand.
    boost::asio::post(
        m_ioc,
        [this, data = std::move(data), srcIp = std::move(srcIp), admission = std::move(admission)]()
        {
            SyslogMessage msg = parseSyslog(data);
            msg.srcIp         = srcIp;
            // Copied rather than moved: the capture is const in a non-mutable
            // lambda, and it costs one refcount either way.
            msg.admission = admission;
            m_outputMgr.dispatch(msg);
            if (m_forwarder != nullptr)
            {
                m_forwarder->forward(msg);
            }
        });
}

void UdpServer::reportDrops(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && m_lastDropReport.has_value() && now - *m_lastDropReport < DROP_REPORT_INTERVAL)
    {
        return;
    }

    const auto dropped = m_admission.takeDropped();
    if (dropped == 0)
    {
        return;
    }

    std::string over;
    if (m_lastDropReport.has_value())
    {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - *m_lastDropReport).count();
        over = ms < 1000 ? " in the last " + std::to_string(ms) + " ms"
                         : " in the last " + std::to_string(ms / 1000) + " s";
    }
    m_lastDropReport = now;

    osLogError("minilog: dropped " + std::to_string(dropped) + " datagram(s)" + over +
               " — the receive queue reached max_queue_bytes (" +
               std::to_string(m_admission.budget()) +
               "). Datagrams are arriving faster than they can be written.");
}

} // namespace minilog
