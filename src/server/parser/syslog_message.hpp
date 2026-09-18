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
#include <memory>
#include <optional>
#include <string>

namespace minilog
{

enum class Protocol
{
    RFC3164,
    RFC5424,
    Unknown
};

struct SyslogMessage
{
    Protocol protocol = Protocol::Unknown;
    std::string raw;   // original datagram bytes (as string)
    std::string srcIp; // sender IP address

    // PRI
    std::optional<int> pri;
    std::optional<int> facility;
    std::optional<int> severity;

    // Named facility/severity (e.g. "daemon", "NOTICE")
    std::optional<std::string> facilityName;
    std::optional<std::string> severityName;

    // Header fields
    std::optional<int> version; // RFC5424 only
    std::optional<std::string> timestamp;
    std::optional<std::string> hostname;
    std::optional<std::string> appName;
    std::optional<std::string> procId;
    std::optional<std::string> msgId; // RFC5424 only

    // Message text
    std::string message;

    // Charge against the receive-queue byte budget, held for as long as any
    // copy of this message is still queued. Opaque here so the parser needs no
    // dependency on AdmissionControl, which is what issues it; see
    // admission.hpp.
    std::shared_ptr<const void> admission;
};

} // namespace minilog
