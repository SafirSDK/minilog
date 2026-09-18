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
#include "service.hpp"

#ifdef _WIN32
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <windows.h>

namespace minilog
{

static constexpr char SERVICE_NAME[]    = "minilog";
static constexpr char SERVICE_DISPLAY[] = "minilog Syslog Server";
static constexpr char SERVICE_DESC[] = "Minimal syslog server. https://github.com/SafirSDK/minilog";

// Upper bound on config load + sink open + socket bind, reported to the SCM as
// the SERVICE_START_PENDING wait hint. Startup is milliseconds in practice; the
// margin is there so a slow or contended disk is not mistaken for a hang.
static constexpr DWORD STARTUP_WAIT_HINT_MS = 10000;

// Recovery actions configured at install time: restart twice, then leave the
// service stopped. The reset period is what separates the two failure modes.
// A service that fails at startup fails again within seconds, so the counter
// keeps climbing and it stops after two attempts, leaving one clearly stopped
// service and a short, readable Event Log rather than an endless restart loop.
// A service that crashes after running healthily for longer than the reset
// period is treated as a fresh first failure each time, so an intermittent
// fault is always retried instead of exhausting its restarts.
static constexpr DWORD RESTART_DELAY_MS       = 5000;
static constexpr DWORD FAILURE_RESET_PERIOD_S = 300;

// ─── Global state shared between SCM callbacks and tryRunAsService ────────────

namespace
{

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent                   = nullptr;
std::function<int()> g_serviceMain;
int g_serviceExitCode = EXIT_FAILURE;

// exitCode is the SCM-visible Win32 status. When it is ERROR_SERVICE_SPECIFIC_ERROR,
// specificExitCode carries minilog's own exit code.
void reportStatus(DWORD state,
                  DWORD exitCode         = NO_ERROR,
                  DWORD waitHint         = 0,
                  DWORD specificExitCode = 0)
{
    static DWORD checkPoint = 1;

    SERVICE_STATUS status{};
    status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState            = state;
    status.dwControlsAccepted        = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;
    status.dwWin32ExitCode           = exitCode;
    status.dwServiceSpecificExitCode = specificExitCode;
    status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;
    status.dwWaitHint   = waitHint;

    SetServiceStatus(g_statusHandle, &status);
}

void WINAPI serviceCtrlHandler(DWORD ctrl)
{
    if (ctrl == SERVICE_CONTROL_STOP)
    {
        reportStatus(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    }
}

void WINAPI serviceMsgMain(DWORD /*argc*/, LPSTR* /*argv*/)
{
    g_statusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME, serviceCtrlHandler);
    if (!g_statusHandle)
    {
        return;
    }

    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        reportStatus(SERVICE_STOPPED, ERROR_OUTOFMEMORY);
        return;
    }

    // Stay in START_PENDING until runServer() has loaded the config, opened the
    // sinks and bound the socket; reportServiceStarted() makes the transition to
    // SERVICE_RUNNING. Reporting RUNNING here instead would make `sc start`
    // succeed even when startup is about to fail.
    reportStatus(SERVICE_START_PENDING, NO_ERROR, STARTUP_WAIT_HINT_MS);

    if (g_serviceMain)
    {
        g_serviceExitCode = g_serviceMain();
    }

    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;

    if (g_serviceExitCode == EXIT_SUCCESS)
    {
        reportStatus(SERVICE_STOPPED);
    }
    else
    {
        // Reporting a non-zero exit code is what tells the SCM this was a failure
        // rather than a clean stop, and is the precondition for the recovery
        // actions configured by installService() to fire. The reason itself is
        // already in the Event Log, written by osLogError().
        reportStatus(SERVICE_STOPPED,
                     ERROR_SERVICE_SPECIFIC_ERROR,
                     0,
                     static_cast<DWORD>(g_serviceExitCode));
    }
}

} // namespace

// ─── Public interface ─────────────────────────────────────────────────────────

void setupShutdown(boost::asio::io_context& ioc, std::function<void()> onStop)
{
    if (g_stopEvent != nullptr)
    {
        // Running as Windows service: watch the stop event on a helper thread
        // so the SCM notification bridges into the io_context work queue.
        HANDLE ev = g_stopEvent;
        std::thread(
            [&ioc, onStop = std::move(onStop), ev]()
            {
                WaitForSingleObject(ev, INFINITE);
                boost::asio::post(ioc, onStop);
            })
            .detach();
    }
    else
    {
        // Running as console process: use Ctrl+C / Ctrl+Break signals.
        // SIGBREAK catches CTRL_BREAK_EVENT from the test harness (which spawns
        // the server with CREATE_NEW_PROCESS_GROUP, disabling CTRL_C/SIGINT).
        auto signals = std::make_shared<boost::asio::signal_set>(ioc, SIGINT, SIGTERM, SIGBREAK);
        signals->async_wait(
            [signals, onStop = std::move(onStop)](const boost::system::error_code& ec,
                                                  int /*signum*/)
            {
                if (!ec)
                {
                    onStop();
                }
            });
    }
}

void reportServiceStarted()
{
    if (g_statusHandle != nullptr)
    {
        reportStatus(SERVICE_RUNNING);
    }
}

std::optional<int> tryRunAsService(const std::function<int()>& serviceMain)
{
    g_serviceMain = serviceMain;

    SERVICE_TABLE_ENTRYA table[] = {
        {const_cast<char*>(SERVICE_NAME), serviceMsgMain},
        {nullptr, nullptr},
    };

    if (!StartServiceCtrlDispatcherA(table))
    {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        {
            // Not started by the SCM — running interactively.
            return std::nullopt;
        }
        throw std::runtime_error("StartServiceCtrlDispatcher failed: " +
                                 std::to_string(GetLastError()));
    }

    return g_serviceExitCode; // SCM invoked us as a service; serviceMain has already run.
}

namespace
{

// The path of the running image, as the OS knows it. GetModuleFileName is the
// only correct answer: std::filesystem::absolute(argv[0]) merely prepends the
// working directory to whatever the caller typed, so an invocation through PATH
// yields "<CWD>\minilog" — a file that does not exist.
std::string currentExecutablePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0)
        {
            throw std::runtime_error("GetModuleFileName failed: " + std::to_string(GetLastError()));
        }
        // written == size means the name was truncated to fit; grow and retry.
        if (written < buffer.size())
        {
            buffer.resize(written);
            return std::filesystem::path(buffer).string();
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace

void installService(const std::string& configPath)
{
    const std::string exePath = currentExecutablePath();

    const SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm)
    {
        throw std::runtime_error("OpenSCManager failed: " + std::to_string(GetLastError()));
    }

    const std::string binPath = "\"" + exePath + "\" \"" + configPath + "\"";

    const SC_HANDLE svc = CreateServiceA(scm,
                                         SERVICE_NAME,
                                         SERVICE_DISPLAY,
                                         SERVICE_ALL_ACCESS,
                                         SERVICE_WIN32_OWN_PROCESS,
                                         SERVICE_AUTO_START,
                                         SERVICE_ERROR_NORMAL,
                                         binPath.c_str(),
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr);
    if (!svc)
    {
        CloseServiceHandle(scm);
        throw std::runtime_error("CreateService failed: " + std::to_string(GetLastError()));
    }

    SERVICE_DESCRIPTIONA desc{const_cast<char*>(SERVICE_DESC)};
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // The SCM repeats the last action for every further failure, so the trailing
    // SC_ACTION_NONE is what stops the restarts after the second attempt.
    SC_ACTION actions[] = {
        {SC_ACTION_RESTART, RESTART_DELAY_MS},
        {SC_ACTION_RESTART, RESTART_DELAY_MS},
        {SC_ACTION_NONE, 0},
    };
    SERVICE_FAILURE_ACTIONSA failureActions{};
    failureActions.dwResetPeriod = FAILURE_RESET_PERIOD_S;
    failureActions.cActions      = static_cast<DWORD>(std::size(actions));
    failureActions.lpsaActions   = actions;
    if (!ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions))
    {
        osLogError("minilog: failed to configure service recovery actions: " +
                   std::to_string(GetLastError()));
    }

    // Without this flag the SCM runs the recovery actions only when the process
    // dies outright. minilog reports its own failures as SERVICE_STOPPED with a
    // non-zero exit code, which counts as a failure only when the flag is set.
    SERVICE_FAILURE_ACTIONS_FLAG failureFlag{TRUE};
    if (!ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlag))
    {
        osLogError("minilog: failed to enable recovery actions on non-crash failures: " +
                   std::to_string(GetLastError()));
    }

    // Register the event source so Event Viewer can display messages from the exe.
    static constexpr char EVENT_LOG_KEY[] =
        "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\minilog";
    HKEY hKey = nullptr;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        EVENT_LOG_KEY,
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE,
                        nullptr,
                        &hKey,
                        nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExA(hKey,
                       "EventMessageFile",
                       0,
                       REG_EXPAND_SZ,
                       reinterpret_cast<const BYTE*>(exePath.c_str()),
                       static_cast<DWORD>(exePath.size() + 1));
        const DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
        RegSetValueExA(hKey,
                       "TypesSupported",
                       0,
                       REG_DWORD,
                       reinterpret_cast<const BYTE*>(&types),
                       sizeof(types));
        RegCloseKey(hKey);
    }

    osLogInfo(std::string("minilog service installed (") + binPath + ")");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

void uninstallService()
{
    const SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
    {
        throw std::runtime_error("OpenSCManager failed: " + std::to_string(GetLastError()));
    }

    const SC_HANDLE svc =
        OpenServiceA(scm, SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc)
    {
        CloseServiceHandle(scm);
        throw std::runtime_error("OpenService failed: " + std::to_string(GetLastError()));
    }

    // Attempt to stop the service before deleting it (best effort).
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    if (!DeleteService(svc))
    {
        const DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        throw std::runtime_error("DeleteService failed: " + std::to_string(err));
    }

    RegDeleteKeyA(HKEY_LOCAL_MACHINE,
                  "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\minilog");

    osLogInfo("minilog service uninstalled");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

} // namespace minilog

#endif // _WIN32
