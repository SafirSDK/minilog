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

#include <boost/asio/io_context.hpp>

#include <functional>
#include <optional>
#include <string>

namespace minilog
{

// Register an async shutdown handler on the io_context.
//
// Linux:   watches SIGTERM and SIGINT via boost::asio::signal_set.
// Windows: watches the SCM SERVICE_STOP event when running as a service,
//          or CTRL+C (SIGINT) when running as a console process.
//
// onStop is called once when a shutdown request is received.
void setupShutdown(boost::asio::io_context& ioc, std::function<void()> onStop);

// Attempt to run the process as a Windows NT service.
//
// Windows: calls StartServiceCtrlDispatcher; if this process was started by
//          the SCM, serviceMain is invoked from the service thread and this
//          function returns the serviceMain exit code when the service exits.
//          Returns std::nullopt if the process was started interactively
//          (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT).
//
// Linux:   always returns std::nullopt immediately.
std::optional<int> tryRunAsService(const std::function<int()>& serviceMain);

// Install minilog as a Windows NT auto-start service.
// exePath    - full path to the minilog executable
// configPath - full path to the config file (passed as a CLI arg to the service)
//
// Throws std::runtime_error on failure.
// Linux: no-op.
void installService(const std::string& exePath, const std::string& configPath);

// Stop and delete the minilog Windows NT service.
// Throws std::runtime_error on failure.
// Linux: no-op.
void uninstallService();

} // namespace minilog
