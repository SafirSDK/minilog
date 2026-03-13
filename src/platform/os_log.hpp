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
#include <string>

namespace minilog
{

// Log a message to the OS facility (Windows Event Log or Linux syslog) and stderr.
void os_log_error(const std::string& message);
void os_log_info(const std::string& message);

} // namespace minilog
