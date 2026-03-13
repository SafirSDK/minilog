#pragma once
#include "syslog_message.hpp"

#include <string_view>

namespace minilog {

// Parse a syslog datagram (RFC3164 or RFC5424).
// Never throws — unknown/malformed input returns Protocol::Unknown.
SyslogMessage parse_syslog(std::string_view data);

} // namespace minilog
