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

// ─── Global state shared between SCM callbacks and tryRunAsService ────────────

namespace
{

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent                   = nullptr;
std::function<int()> g_serviceMain;
int g_serviceExitCode = EXIT_FAILURE;

void reportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0)
{
    static DWORD checkPoint = 1;

    SERVICE_STATUS status{};
    status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState     = state;
    status.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;
    status.dwWin32ExitCode    = exitCode;
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

    reportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    reportStatus(SERVICE_RUNNING);

    if (g_serviceMain)
    {
        g_serviceExitCode = g_serviceMain();
    }

    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    reportStatus(SERVICE_STOPPED);
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

void installService(const std::string& exePath, const std::string& configPath)
{
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
