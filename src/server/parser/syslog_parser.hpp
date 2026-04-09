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
#include "syslog_message.hpp"

#include <string_view>

namespace minilog
{

// Parse a syslog datagram (RFC3164 or RFC5424).
// Never throws — unknown/malformed input returns Protocol::Unknown.
SyslogMessage parseSyslog(std::string_view data);

} // namespace minilog
