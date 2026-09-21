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

#include "preflight.hpp"

#include "config/config.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ostream>

namespace minilog
{

namespace
{

namespace fs = std::filesystem;

void addError(std::vector<Finding>& out, std::string message)
{
    out.push_back({Finding::Level::Error, std::move(message)});
}

void addWarning(std::vector<Finding>& out, std::string message)
{
    out.push_back({Finding::Level::Warning, std::move(message)});
}

std::string endpointText(const std::string& host, uint16_t port)
{
    return host + ":" + std::to_string(port);
}

// A name for a file that does not exist yet, inside the directory being tested.
//
// Unique per call rather than fixed: two --check runs at once must not have one
// delete the other's probe and conclude the directory is unwritable. The clock
// is only a source of distinct names here, so its epoch and resolution do not
// matter.
fs::path probePath(const fs::path& dir)
{
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return dir /
           (".minilog-check-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
}

// Can minilog create and write files in this directory?
//
// Tested by writing a probe file and deleting it again, not by opening the
// configured log file: opening that would leave an empty syslog.log behind on
// every successful run, and a --check that changes the machine it is inspecting
// is not a --check.
void checkDirectory(const fs::path& dir, std::vector<Finding>& out)
{
    std::error_code ec;
    if (!fs::exists(dir, ec))
    {
        if (ec)
        {
            // Almost always a parent directory that cannot be searched. Saying
            // "does not exist" here would send someone off to create a
            // directory that is already there.
            addError(out,
                     "cannot inspect log directory '" + dir.string() + "': " + ec.message() +
                         " — check the permissions on the path leading to it");
            return;
        }
        addError(out,
                 "log directory '" + dir.string() +
                     "' does not exist — minilog never creates directories, so it has to be "
                     "provisioned before minilog runs");
        return;
    }

    if (!fs::is_directory(dir, ec))
    {
        addError(out, "'" + dir.string() + "' is not a directory");
        return;
    }

    const auto probe = probePath(dir);
    {
        const std::ofstream probeFile(probe);
        if (!probeFile.good())
        {
            // The distinction from the missing case above is most of the value of
            // --check in a locked-down deployment: one needs a directory created,
            // the other needs an ACL changed, and nothing else tells them apart.
            addError(out,
                     "log directory '" + dir.string() +
                         "' exists but is not writable by this user — minilog will open no log "
                         "file in it and will silently discard every message routed there");
            return;
        }
    }
    fs::remove(probe, ec);
    if (ec)
    {
        // Writable but not deletable means a sticky directory or a restrictive
        // ACL, which is not itself fatal — minilog only ever appends. Worth
        // saying because rotation does delete the oldest generation.
        addWarning(out,
                   "wrote but could not delete '" + probe.string() + "' (" + ec.message() +
                       "); log rotation deletes old generations and will fail the same way");
    }
}

// Only for files that are already there: appending to an existing log file can
// fail on its own ACL even where the directory is fine, and that is exactly the
// state a machine reaches after someone tightens permissions on yesterday's logs.
void checkExistingFile(const std::string& path, std::vector<Finding>& out)
{
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
    {
        return; // Nothing to test yet; the directory check covers creating it.
    }

    if (fs::is_directory(path, ec))
    {
        addError(out, "'" + path + "' is a directory, not a log file");
        return;
    }

    const std::ofstream probe(path, std::ios::app);
    if (!probe.good())
    {
        addError(out,
                 "log file '" + path +
                     "' exists but cannot be opened for appending — minilog will close that sink "
                     "at startup and discard the messages routed to it");
    }
}

// How a failed bind should be read depends entirely on what else is running.
Finding
classifyBindFailure(const std::string& endpoint, const std::string& reason, ServiceState service)
{
    if (service == ServiceState::Running)
    {
        // The overwhelmingly common case: --check is run to diagnose a machine
        // that already has minilog installed and running. Reporting its own
        // socket as a fault would make the healthy state look broken.
        return {Finding::Level::Warning,
                "UDP " + endpoint +
                    " is held by the minilog service, which is running on this machine — that is "
                    "expected, and it means --check cannot test the bind itself (" +
                    reason + ")"};
    }

    std::string message = "cannot bind UDP " + endpoint + ": " + reason;
    if (service == ServiceState::Unknown)
    {
        message += " — the service manager could not be asked whether an already running minilog "
                   "holds it";
    }
    return {Finding::Level::Error, std::move(message)};
}

// Bind the configured listen endpoint with the same socket options the server
// uses, then drop it again. This is the only check that can be made wrong by
// something outside minilog's control, so it is also the one that most needs to
// be read alongside the service state.
//
// It proves that minilog can take the port on this host. It proves nothing about
// whether remote senders can reach it: that needs a datagram from a real sender,
// and no local test can establish that a firewall rule exists.
void checkBind(const Config& cfg, ServiceState service, std::vector<Finding>& out)
{
    using udp = boost::asio::ip::udp;

    const auto endpoint = endpointText(cfg.host, cfg.udpPort);

    boost::system::error_code addrEc;
    const auto address = boost::asio::ip::make_address(cfg.host, addrEc);
    if (addrEc)
    {
        // loadConfig validates the host, so reaching this means the two
        // disagree; report it rather than letting make_address throw.
        addError(out, "invalid [server] host '" + cfg.host + "': " + addrEc.message());
        return;
    }

    boost::asio::io_context ioc;
    udp::socket socket(ioc);
    try
    {
        const udp::endpoint ep(address, cfg.udpPort);
        socket.open(ep.protocol());
#ifdef _WIN32
        // Without this the bind would succeed even with minilog running, because
        // Windows shares UDP ports by default — and a --check that passes where
        // the server would fail is worse than no check at all.
        const BOOL exclusive = TRUE;
        setsockopt(socket.native_handle(),
                   SOL_SOCKET,
                   SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive));
#endif
        socket.bind(ep);
    }
    catch (const boost::system::system_error& e)
    {
        out.push_back(classifyBindFailure(endpoint, e.code().message(), service));
    }
}

// Resolve the forwarding destination synchronously.
//
// The server deliberately does not: a name that does not resolve must not fail a
// service start, so Forwarder looks it up in the background and retries. That is
// right at runtime and useless at preflight time, where waiting for the resolver
// is the whole point.
void checkForwarding(const ForwardingConfig& fwd, std::vector<Finding>& out)
{
    if (!fwd.enabled)
    {
        return;
    }

    boost::asio::io_context ioc;
    boost::asio::ip::udp::resolver resolver(ioc);
    boost::system::error_code ec;
    const auto results = resolver.resolve(fwd.host, std::to_string(fwd.port), ec);
    if (ec || results.empty())
    {
        addError(out,
                 "forwarding is enabled but destination '" + fwd.host + "' does not resolve" +
                     (ec ? ": " + ec.message() : "") +
                     " — minilog still starts and retries the lookup in the background, so this "
                     "shows up as nothing being forwarded rather than as a startup failure");
    }
}

} // namespace

uint64_t PreflightReport::count(Finding::Level level) const
{
    return static_cast<uint64_t>(std::count_if(
        findings.begin(), findings.end(), [level](const Finding& f) { return f.level == level; }));
}

PreflightReport preflight(const std::string& configPath, ServiceState service)
{
    PreflightReport report;

    Config cfg;
    try
    {
        cfg = loadConfig(configPath);
    }
    catch (const std::exception& e)
    {
        // The only early return in here: every later check reads the config, so
        // there is nothing left to say about the machine once it will not load.
        addError(report.findings, std::string("config: ") + e.what());
        return report;
    }

    report.requirements.push_back("inbound UDP datagrams on " +
                                  endpointText(cfg.host, cfg.udpPort));

    std::vector<fs::path> directories;
    for (const auto& out : cfg.outputs)
    {
        for (const auto* path : {&out.textFile, &out.jsonlFile})
        {
            if (path->empty())
            {
                continue;
            }
            const auto dir = fs::path(*path).parent_path();
            if (std::find(directories.begin(), directories.end(), dir) == directories.end())
            {
                directories.push_back(dir);
            }
            checkExistingFile(*path, report.findings);
        }

        if (out.maxSize == 0)
        {
            addWarning(report.findings,
                       "[output." + out.name +
                           "] has max_size = 0, so its files are never rotated and grow until the "
                           "filesystem is full");
        }
    }

    for (const auto& dir : directories)
    {
        report.requirements.push_back("write access to directory " + dir.string());
        checkDirectory(dir, report.findings);
    }

    if (cfg.forwarding.enabled)
    {
        report.requirements.push_back("outbound UDP to " +
                                      endpointText(cfg.forwarding.host, cfg.forwarding.port) +
                                      " (forwarding)");
    }

    checkBind(cfg, service, report.findings);
    checkForwarding(cfg.forwarding, report.findings);

    return report;
}

int printPreflight(const PreflightReport& report, const std::string& configPath, std::ostream& out)
{
    out << "minilog --check " << configPath << "\n";

    if (!report.requirements.empty())
    {
        out << "\nThis configuration requires:\n";
        for (const auto& requirement : report.requirements)
        {
            out << "  - " << requirement << "\n";
        }
        out << "\nWhether remote senders can reach the listen port is not checked: only a "
               "datagram\n"
               "from a real sender can show that the firewall rules allow it.\n";
    }

    if (report.findings.empty())
    {
        out << "\nNo problems found.\n";
        return EXIT_SUCCESS;
    }

    out << "\n";
    for (const auto& finding : report.findings)
    {
        out << (finding.level == Finding::Level::Error ? "ERROR:   " : "WARNING: ")
            << finding.message << "\n";
    }

    const auto errors   = report.count(Finding::Level::Error);
    const auto warnings = report.count(Finding::Level::Warning);
    out << "\n" << errors << " error(s), " << warnings << " warning(s)\n";

    return report.failed() ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace minilog
