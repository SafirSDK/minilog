#pragma once
#include <optional>
#include <string>

namespace minilog {

enum class Protocol { RFC3164, RFC5424, Unknown };

struct SyslogMessage {
    Protocol protocol = Protocol::Unknown;
    std::string raw;    // original datagram bytes (as string)
    std::string src_ip; // sender IP address

    // PRI
    std::optional<int> pri;
    std::optional<int> facility;
    std::optional<int> severity;

    // Named facility/severity (e.g. "daemon", "NOTICE")
    std::optional<std::string> facility_name;
    std::optional<std::string> severity_name;

    // Header fields
    std::optional<int> version; // RFC5424 only
    std::optional<std::string> timestamp;
    std::optional<std::string> hostname;
    std::optional<std::string> app_name;
    std::optional<std::string> proc_id;
    std::optional<std::string> msg_id; // RFC5424 only

    // Message text
    std::string message;
};

} // namespace minilog
