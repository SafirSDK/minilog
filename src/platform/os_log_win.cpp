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
#ifdef _WIN32
#include <windows.h>
#endif

namespace minilog
{

void osLogError(const std::string& message)
{
#ifdef _WIN32
    HANDLE h = RegisterEventSourceA(nullptr, "minilog");
    if (h)
    {
        const char* msg = message.c_str();
        ReportEventA(h, EVENTLOG_ERROR_TYPE, 0, 0, nullptr, 1, 0, &msg, nullptr);
        DeregisterEventSource(h);
    }
#endif
    std::cerr << "[ERROR] " << message << "\n";
}

void osLogInfo(const std::string& message)
{
#ifdef _WIN32
    HANDLE h = RegisterEventSourceA(nullptr, "minilog");
    if (h)
    {
        const char* msg = message.c_str();
        ReportEventA(h, EVENTLOG_INFORMATION_TYPE, 0, 0, nullptr, 1, 0, &msg, nullptr);
        DeregisterEventSource(h);
    }
#endif
    std::cerr << "[INFO] " << message << "\n";
}

} // namespace minilog
