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
#include "wait_until.hpp"

#ifdef _WIN32
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <windows.h>

namespace minilog
{

static constexpr char SERVICE_NAME[]    = "minilog";
static constexpr char SERVICE_DISPLAY[] = "minilog Syslog Server";
static constexpr char SERVICE_DESC[] = "Minimal syslog server. https://github.com/SafirSDK/minilog";

// Upper bound on everything runServer does before reportServiceStarted(): the
// config load, opening the sinks, constructing the forwarder and binding the UDP
// socket. Reported to the SCM as the SERVICE_START_PENDING wait hint. Startup is
// milliseconds in practice; the margin is there so a slow or contended disk is
// not mistaken for a hang.
//
// Resolving the forwarding destination is deliberately *not* in that list. It
// was, as a blocking getaddrinfo in the Forwarder's constructor, and an
// unresponsive resolver blocks there for tens of seconds — long enough to
// exhaust this hint on a start that was otherwise healthy. The lookup now runs
// on the io_context and finishes after the service is already reported running.
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

// How often the SCM is asked whether the service has stopped yet. Short enough
// that stopping a healthy service feels immediate, long enough not to spin.
static constexpr auto STOP_POLL_INTERVAL = std::chrono::milliseconds(200);

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

// Handles are closed by these rather than by hand: stopping a service has
// several failure exits, and each one would otherwise need its own pair of
// CloseServiceHandle calls — which is how a leak gets in.
struct ServiceHandleCloser
{
    void operator()(SC_HANDLE handle) const noexcept { CloseServiceHandle(handle); }
};
using ScopedServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceHandleCloser>;

// Not a unique_ptr: HANDLE is void*, and std::unique_ptr<void, D> is not
// portable across standard libraries.
class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
    ~ScopedHandle()
    {
        if (m_handle != nullptr)
        {
            CloseHandle(m_handle);
        }
    }

    ScopedHandle(const ScopedHandle&)            = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return m_handle; }
    explicit operator bool() const { return m_handle != nullptr; }

private:
    HANDLE m_handle;
};

SERVICE_STATUS_PROCESS queryServiceStatus(SC_HANDLE svc)
{
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc,
                              SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status),
                              static_cast<DWORD>(sizeof(status)),
                              &needed))
    {
        throw std::runtime_error("QueryServiceStatusEx failed: " + std::to_string(GetLastError()));
    }
    return status;
}

// Ask the SCM to stop the service.
//
// Returns false while the service is not yet in a state where it can accept the
// control — a service that is still starting answers every stop with
// ERROR_SERVICE_CANNOT_ACCEPT_CTRL until it reports RUNNING. That is a real
// case here rather than a theoretical one: the recovery actions configured by
// installService restart the service five seconds after a failure, so an
// upgrade that runs --stop inside that window meets a starting service.
//
// Throws on anything else; a service that is already stopped is success.
bool requestStop(SC_HANDLE svc)
{
    SERVICE_STATUS status{};
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status))
    {
        return true;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_SERVICE_NOT_ACTIVE)
    {
        return true;
    }
    if (err == ERROR_SERVICE_CANNOT_ACCEPT_CTRL)
    {
        return false;
    }
    throw std::runtime_error("failed to stop the minilog service: " + std::to_string(err));
}

// Ask the service to stop, and do not return until its process has exited.
//
// Two waits, because the SCM state and the process are not the same thing. A
// service reports SERVICE_STOPPED from its own stop handler, before it has
// unwound and released its image file, and a running executable can be neither
// overwritten nor deleted. Callers about to copy a new build over the old one,
// or to delete the service, care about the second wait, not the first.
//
// Throws if the service is still there when the timeout runs out — better a
// caller that knows the stop failed than one that goes on to overwrite a file
// still in use.
void stopAndWait(SC_HANDLE svc, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    const SERVICE_STATUS_PROCESS initial = queryServiceStatus(svc);
    if (initial.dwCurrentState == SERVICE_STOPPED)
    {
        return;
    }

    // Opened before the stop is requested: once the process has exited its id
    // means nothing, and may already have been reused by something else.
    const ScopedHandle process(
        initial.dwProcessId != 0 ? OpenProcess(SYNCHRONIZE, FALSE, initial.dwProcessId) : nullptr);
    const DWORD openError = (initial.dwProcessId != 0 && !process) ? GetLastError() : 0;
    if (!process)
    {
        // Said out loud, because it downgrades what this function promises: the
        // caller is about to overwrite or delete a file on the strength of the
        // process being gone, and without a handle only the SCM's word is left.
        osLogInfo("minilog: waiting on the service state alone, not on process exit (" +
                  (openError != 0 ? "OpenProcess failed: " + std::to_string(openError)
                                  : std::string("the SCM reported no process id")) +
                  ")");
    }

    if (initial.dwCurrentState != SERVICE_STOP_PENDING)
    {
        if (!waitUntil([svc] { return requestStop(svc); },
                       deadline - std::chrono::steady_clock::now(),
                       STOP_POLL_INTERVAL))
        {
            throw std::runtime_error("the minilog service was still starting and would not "
                                     "accept a stop within " +
                                     std::to_string(timeout.count()) + " s");
        }
    }

    // ControlService is asynchronous: it returns with the service still in
    // SERVICE_STOP_PENDING.
    if (!waitUntil([svc] { return queryServiceStatus(svc).dwCurrentState == SERVICE_STOPPED; },
                   deadline - std::chrono::steady_clock::now(),
                   STOP_POLL_INTERVAL))
    {
        throw std::runtime_error("the minilog service did not stop within " +
                                 std::to_string(timeout.count()) + " s");
    }

    if (!process)
    {
        // Nothing to wait on; the caveat was reported above.
        return;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const DWORD remainingMs = remaining.count() > 0 ? static_cast<DWORD>(remaining.count()) : 0;
    if (WaitForSingleObject(process.get(), remainingMs) != WAIT_OBJECT_0)
    {
        throw std::runtime_error(
            "the minilog service reported itself stopped but its process was still "
            "running after " +
            std::to_string(timeout.count()) + " s");
    }
}

} // namespace

void installService(const std::string& configPath)
{
    const std::string exePath = currentExecutablePath();

    const ScopedServiceHandle scm(
        OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm)
    {
        throw std::runtime_error("OpenSCManager failed: " + std::to_string(GetLastError()));
    }

    const std::string binPath = "\"" + exePath + "\" \"" + configPath + "\"";

    ScopedServiceHandle svc(CreateServiceA(scm.get(),
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
                                           nullptr));

    const bool created = static_cast<bool>(svc);
    if (!created)
    {
        if (GetLastError() != ERROR_SERVICE_EXISTS)
        {
            throw std::runtime_error("CreateService failed: " + std::to_string(GetLastError()));
        }

        // Already registered: update the registration rather than fail, so that
        // "make this service point at this executable and this config" is a
        // single command an upgrade can run. SERVICE_START is needed alongside
        // SERVICE_CHANGE_CONFIG because the recovery actions below restart the
        // service.
        svc.reset(OpenServiceA(scm.get(), SERVICE_NAME, SERVICE_CHANGE_CONFIG | SERVICE_START));
        if (!svc)
        {
            throw std::runtime_error("OpenService failed: " + std::to_string(GetLastError()));
        }

        // The binary path is always updated — the executable may have moved and
        // the config path may have changed, which is the whole point of running
        // --install again. The start type and the account are deliberately left
        // alone: an administrator who set the service to manual start or bound
        // it to a specific account must not have that undone by an upgrade.
        if (!ChangeServiceConfigA(svc.get(),
                                  SERVICE_NO_CHANGE,
                                  SERVICE_NO_CHANGE,
                                  SERVICE_NO_CHANGE,
                                  binPath.c_str(),
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  SERVICE_DISPLAY))
        {
            throw std::runtime_error("ChangeServiceConfig failed: " +
                                     std::to_string(GetLastError()));
        }
    }

    SERVICE_DESCRIPTIONA desc{const_cast<char*>(SERVICE_DESC)};
    ChangeServiceConfig2A(svc.get(), SERVICE_CONFIG_DESCRIPTION, &desc);

    // The SCM repeats the last action for every further failure, so the trailing
    // SC_ACTION_NONE is what stops the restarts after the second attempt.
    SC_ACTION actions[] = {
        {SC_ACTION_RESTART, RESTART_DELAY_MS},
        {SC_ACTION_RESTART, RESTART_DELAY_MS},
        {SC_ACTION_NONE, 0},
    };
    // Re-applied on update as well as on create: an installation upgraded from a
    // minilog that predates them would otherwise never gain them. Unlike the
    // account and the start type, these are minilog's own behaviour rather than
    // a deployment decision.
    SERVICE_FAILURE_ACTIONSA failureActions{};
    failureActions.dwResetPeriod = FAILURE_RESET_PERIOD_S;
    failureActions.cActions      = static_cast<DWORD>(std::size(actions));
    failureActions.lpsaActions   = actions;
    if (!ChangeServiceConfig2A(svc.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions))
    {
        osLogError("minilog: failed to configure service recovery actions: " +
                   std::to_string(GetLastError()));
    }

    // Without this flag the SCM runs the recovery actions only when the process
    // dies outright. minilog reports its own failures as SERVICE_STOPPED with a
    // non-zero exit code, which counts as a failure only when the flag is set.
    SERVICE_FAILURE_ACTIONS_FLAG failureFlag{TRUE};
    if (!ChangeServiceConfig2A(svc.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlag))
    {
        osLogError("minilog: failed to enable recovery actions on non-crash failures: " +
                   std::to_string(GetLastError()));
    }

    // Rewritten on every install: the value records the path of the executable,
    // so after an upgrade that moved the binary a stale one leaves Event Viewer
    // showing "description not found" instead of the message.
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

    if (created)
    {
        osLogInfo("minilog service installed (" + binPath + ")");
    }
    else
    {
        osLogInfo("minilog service updated (" + binPath +
                  ") — restart the service to run the new registration");
    }
}

void stopService(std::chrono::seconds timeout)
{
    const ScopedServiceHandle scm(OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
    {
        throw std::runtime_error("OpenSCManager failed: " + std::to_string(GetLastError()));
    }

    const ScopedServiceHandle svc(
        OpenServiceA(scm.get(), SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!svc)
    {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            // Nothing registered is nothing to stop. An upgrade script should
            // not have to know whether this machine has minilog on it yet.
            osLogInfo("minilog service is not registered; nothing to stop");
            return;
        }
        throw std::runtime_error("OpenService failed: " + std::to_string(GetLastError()));
    }

    stopAndWait(svc.get(), timeout);
    osLogInfo("minilog service stopped");
}

void uninstallService(std::chrono::seconds timeout)
{
    const ScopedServiceHandle scm(OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
    {
        throw std::runtime_error("OpenSCManager failed: " + std::to_string(GetLastError()));
    }

    const ScopedServiceHandle svc(
        OpenServiceA(scm.get(), SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS));
    if (svc)
    {
        // Deleting a service that is still running only marks it for deletion:
        // the registration lingers, and the next CreateService fails with
        // ERROR_SERVICE_MARKED_FOR_DELETE. Waiting here is what makes an install
        // that follows an uninstall reliable.
        stopAndWait(svc.get(), timeout);

        if (!DeleteService(svc.get()))
        {
            throw std::runtime_error("DeleteService failed: " + std::to_string(GetLastError()));
        }
    }
    else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
    {
        throw std::runtime_error("OpenService failed: " + std::to_string(GetLastError()));
    }

    // Logged before the event source is removed, so this entry still reaches the
    // Event Log with a message file to render it.
    osLogInfo(svc ? "minilog service uninstalled"
                  : "minilog service is not registered; nothing to remove");

    // Part of the registration, so it goes whether or not the service itself was
    // still there — an interrupted uninstall must not leave it behind.
    RegDeleteKeyA(HKEY_LOCAL_MACHINE,
                  "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\minilog");
}

} // namespace minilog

#endif // _WIN32
