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

#include <chrono>
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

// Report that startup has completed successfully.
//
// Windows: transitions the service from SERVICE_START_PENDING to
//          SERVICE_RUNNING. Until this is called the SCM considers the service
//          to be still starting, so a startup that fails before reaching this
//          point makes `sc start` fail instead of reporting success.
//          No-op when the process was not started by the SCM.
//
// Linux:   no-op.
void reportServiceStarted();

// Attempt to run the process as a Windows NT service.
//
// Windows: calls StartServiceCtrlDispatcher; if this process was started by
//          the SCM, serviceMain is invoked from the service thread and this
//          function returns the serviceMain exit code when the service exits.
//          A non-zero serviceMain exit code is reported to the SCM as a
//          service-specific failure, which is what makes the recovery actions
//          configured by installService() fire.
//          Returns std::nullopt if the process was started interactively
//          (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT).
//
// Linux:   always returns std::nullopt immediately.
std::optional<int> tryRunAsService(const std::function<int()>& serviceMain);

// Install minilog as a Windows NT auto-start service, with recovery actions
// that restart it twice before giving up.
// configPath - absolute path to the config file, passed as a CLI arg to the
//              service. Absolute because the SCM starts services with the
//              working directory set to System32, where a relative path would
//              resolve to something else entirely.
//
// The path to the executable is not passed in: it is read from the OS with
// GetModuleFileName, which is correct however the process was invoked. Deriving
// it from argv[0] is not — argv[0] is whatever the caller typed, and is just
// "minilog" when the executable is found through PATH.
//
// Throws std::runtime_error on failure.
// Linux: no-op.
void installService(const std::string& configPath);

// Stop the minilog Windows NT service and wait until its process has exited.
// timeout - how long to wait before giving up.
//
// Waiting on process exit rather than on the SCM state is the whole point: a
// service reports SERVICE_STOPPED before its process returns from main, and a
// running executable cannot be overwritten. This is the primitive an upgrade
// needs between stopping the old build and copying the new one.
//
// A service that is not registered, or already stopped, is success: the state
// the caller asked for already holds.
//
// Throws std::runtime_error if the service does not stop in time, or on any
// other SCM failure.
// Linux: no-op.
void stopService(std::chrono::seconds timeout);

// Stop the minilog Windows NT service, wait for its process to exit, and delete
// it. timeout - how long to wait for the stop.
//
// The wait is not optional: DeleteService on a service that is still running
// only *marks* it for deletion, and the next CreateService then fails with
// ERROR_SERVICE_MARKED_FOR_DELETE.
//
// Throws std::runtime_error on failure.
// Linux: no-op.
void uninstallService(std::chrono::seconds timeout);

} // namespace minilog
