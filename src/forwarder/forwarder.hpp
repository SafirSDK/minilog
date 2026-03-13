#pragma once
#include "config/config.hpp"
#include "parser/syslog_message.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

namespace minilog {

// Forwards messages to a remote syslog server over UDP.
// Messages exceeding max_message_size are truncated before sending.
class Forwarder {
public:
    Forwarder(boost::asio::io_context& ioc, const ForwardingConfig& cfg);

    // Dispatch a forward to this forwarder's strand (non-blocking for caller).
    void forward(const SyslogMessage& msg);

private:
    void do_forward(const SyslogMessage& msg);

    ForwardingConfig cfg_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
};

} // namespace minilog
