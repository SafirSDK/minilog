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

#include "os_log.hpp"

#include <iostream>
#include <syslog.h>

namespace minilog
{

void osLogError(const std::string& message)
{
    syslog(LOG_ERR, "%s", message.c_str());
    std::cerr << "[ERROR] " << message << "\n";
}

void osLogInfo(const std::string& message)
{
    syslog(LOG_INFO, "%s", message.c_str());
    std::cerr << "[INFO] " << message << "\n";
}

} // namespace minilog
