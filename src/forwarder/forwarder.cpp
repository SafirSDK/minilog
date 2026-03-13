#include "forwarder.hpp"

namespace minilog
{

Forwarder::Forwarder(boost::asio::io_context& ioc, const ForwardingConfig& cfg)
    : m_cfg(cfg), m_strand(boost::asio::make_strand(ioc))
{
}

void Forwarder::forward(const SyslogMessage& msg)
{
    boost::asio::post(m_strand, [this, msg]() { do_forward(msg); });
}

void Forwarder::do_forward(const SyslogMessage& /*msg*/)
{
    // TODO: truncate if needed, send UDP datagram
}

} // namespace minilog
