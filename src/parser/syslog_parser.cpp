#include "syslog_parser.hpp"

namespace minilog {

SyslogMessage parse_syslog(std::string_view data)
{
    // TODO: implement RFC3164 + RFC5424 parsing
    SyslogMessage msg;
    msg.protocol = Protocol::Unknown;
    msg.raw      = std::string(data);
    msg.message  = std::string(data);
    return msg;
}

} // namespace minilog
