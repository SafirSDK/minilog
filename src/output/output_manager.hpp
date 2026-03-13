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
#include "log_file.hpp"

#include "config/config.hpp"
#include "parser/syslog_message.hpp"

#include <boost/asio/io_context.hpp>

#include <memory>
#include <vector>

namespace minilog
{

// Routes each received message to all matching OutputConfig sinks.
class OutputManager
{
public:
    OutputManager(boost::asio::io_context& ioc, const Config& cfg);

    // Dispatch message to all matching sinks.
    void dispatch(const SyslogMessage& msg);

    // Shut down all sinks.
    void close();

private:
    std::vector<std::unique_ptr<LogFile>> m_sinks;
};

} // namespace minilog
