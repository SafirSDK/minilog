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

#define BOOST_TEST_MODULE test_preflight
#include "preflight.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace minilog;

namespace fs = std::filesystem;

namespace
{

// An otherwise valid configuration, so each test varies exactly one thing.
struct Options
{
    std::string logDir;           // empty = the fixture directory
    std::string maxSize = "10MB"; // "0" disables rotation
    uint16_t udpPort    = 0;      // 0 = pick a free one
    std::string forwardHost;      // empty = forwarding disabled
    bool secondSinkSameDir = false;
};

// A directory of its own per test, removed afterwards. Everything here inspects
// the real filesystem: that is the whole point of --check, so there is nothing
// to fake.
struct Fixture
{
    fs::path dir;

    Fixture()
    {
        static std::atomic<int> counter{0};
        dir = fs::temp_directory_path() /
              ("minilog_preflight_" + std::to_string(counter.fetch_add(1)) + "_" +
               std::to_string(static_cast<unsigned long long>(
                   std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(dir);
    }

    ~Fixture()
    {
        std::error_code ec;
#ifndef _WIN32
        // A test that dropped the write bit to prove a point would otherwise
        // leave a directory nothing can remove.
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
        {
            fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add, ec);
        }
#endif
        fs::remove_all(dir, ec);
    }

    Fixture(const Fixture&)            = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::string writeConfig(const Options& opts = {}) const
    {
        const auto logDir = opts.logDir.empty() ? dir.string() : opts.logDir;
        const auto path   = (dir / "minilog.conf").string();

        std::ostringstream ini;
        ini << "[server]\n"
            << "host = 127.0.0.1\n"
            << "udp_port = " << (opts.udpPort != 0 ? opts.udpPort : freePort()) << "\n"
            << "\n[output.main]\n"
            << "text_file = " << (fs::path(logDir) / "syslog.log").string() << "\n"
            << "max_size = " << opts.maxSize << "\n";
        if (opts.secondSinkSameDir)
        {
            ini << "\n[output.audit]\n"
                << "jsonl_file = " << (fs::path(logDir) / "audit.jsonl").string() << "\n"
                << "max_size = 10MB\n";
        }
        if (!opts.forwardHost.empty())
        {
            ini << "\n[forwarding]\n"
                << "enabled = true\n"
                << "host = " << opts.forwardHost << "\n"
                << "port = 1514\n";
        }

        std::ofstream f(path);
        BOOST_REQUIRE(f.good());
        f << ini.str();
        return path;
    }

    // A UDP port nothing is listening on, found by binding and letting go.
    // Racy in principle; the alternative is a hard-coded port, which is worse.
    static uint16_t freePort()
    {
        boost::asio::io_context ioc;
        boost::asio::ip::udp::socket probe(
            ioc, {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(0)});
        return probe.local_endpoint().port();
    }
};

bool mentions(const PreflightReport& report, Finding::Level level, const std::string& needle)
{
    for (const auto& finding : report.findings)
    {
        if (finding.level == level && finding.message.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::string allFindings(const PreflightReport& report)
{
    std::string text;
    for (const auto& finding : report.findings)
    {
        text += (finding.level == Finding::Level::Error ? "[E] " : "[W] ") + finding.message + "\n";
    }
    return text;
}

bool asksFor(const PreflightReport& report, const std::string& needle)
{
    for (const auto& requirement : report.requirements)
    {
        if (requirement.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

#ifndef _WIN32
// Dropping a permission bit only proves anything where it is enforced: POSIX,
// and not as root.
bool canDenyAccess()
{
    return geteuid() != 0;
}
#endif

} // namespace

// ─── The happy path ──────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(healthy_machine, Fixture)

BOOST_AUTO_TEST_CASE(a_valid_configuration_on_a_good_machine_reports_nothing)
{
    const auto report = preflight(writeConfig(), ServiceState::NotInstalled);
    BOOST_TEST(allFindings(report) == "");
    BOOST_TEST(!report.failed());
}

BOOST_AUTO_TEST_CASE(the_requirements_name_the_listen_endpoint_and_every_directory)
{
    const auto report = preflight(writeConfig(), ServiceState::NotInstalled);
    BOOST_TEST(asksFor(report, "inbound UDP datagrams on 127.0.0.1:"));
    BOOST_TEST(asksFor(report, "write access to directory " + dir.string()));
}

BOOST_AUTO_TEST_CASE(two_sinks_in_one_directory_ask_for_write_access_once)
{
    Options opts;
    opts.secondSinkSameDir = true;
    const auto report      = preflight(writeConfig(opts), ServiceState::NotInstalled);

    int seen = 0;
    for (const auto& requirement : report.requirements)
    {
        seen += requirement.find("write access") != std::string::npos ? 1 : 0;
    }
    BOOST_TEST(seen == 1);
    BOOST_TEST(allFindings(report) == "");
}

// The reason the directory test writes a probe file instead of opening the
// configured log file: a validation run that leaves an empty syslog.log behind
// has changed the machine it was asked to inspect.
BOOST_AUTO_TEST_CASE(checking_creates_no_file_that_outlives_the_call)
{
    const auto configPath = writeConfig();
    const auto report     = preflight(configPath, ServiceState::NotInstalled);
    BOOST_TEST(allFindings(report) == "");

    for (const auto& entry : fs::directory_iterator(dir))
    {
        BOOST_TEST(entry.path().string() == configPath);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Config that will not load ───────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(unloadable_config, Fixture)

BOOST_AUTO_TEST_CASE(a_missing_config_file_is_the_only_finding)
{
    const auto report = preflight((dir / "absent.conf").string(), ServiceState::NotInstalled);
    BOOST_TEST(report.failed());
    BOOST_TEST(report.findings.size() == 1U);
    BOOST_TEST(mentions(report, Finding::Level::Error, "config:"));
}

// Nothing else can be checked against a config that will not load, so the
// machine checks are skipped rather than run against defaults.
BOOST_AUTO_TEST_CASE(an_invalid_config_value_stops_the_rest_of_the_checks)
{
    const auto path = (dir / "bad.conf").string();
    {
        std::ofstream f(path);
        f << "[server]\nudp_port = not-a-number\n";
    }

    const auto report = preflight(path, ServiceState::NotInstalled);
    BOOST_TEST(report.failed());
    BOOST_TEST(report.findings.size() == 1U);
    BOOST_TEST(report.requirements.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Directories and files ───────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(log_paths, Fixture)

// "Create the directory" and "fix the ACL" are different jobs, often for
// different people, so the two failures must not read the same.
BOOST_AUTO_TEST_CASE(a_missing_log_directory_says_it_does_not_exist)
{
    Options opts;
    opts.logDir       = (dir / "not-provisioned").string();
    const auto report = preflight(writeConfig(opts), ServiceState::NotInstalled);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "does not exist"));
    BOOST_TEST(mentions(report, Finding::Level::Error, "not-provisioned"));
}

BOOST_AUTO_TEST_CASE(a_log_path_that_is_a_directory_is_reported_as_such)
{
    fs::create_directories(dir / "syslog.log");
    const auto report = preflight(writeConfig(), ServiceState::NotInstalled);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "is a directory, not a log file"));
}

BOOST_AUTO_TEST_CASE(a_log_directory_that_is_really_a_file_is_reported_as_such)
{
    const auto logDir = dir / "occupied";
    std::ofstream(logDir) << "not a directory\n";
    Options opts;
    opts.logDir       = logDir.string();
    const auto report = preflight(writeConfig(opts), ServiceState::NotInstalled);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "is not a directory"));
}

// The no-side-effects promise matters most on the Windows deployment target, so
// this stays outside the POSIX-only block below.
BOOST_AUTO_TEST_CASE(an_existing_writable_log_file_is_left_exactly_as_it_was)
{
    const auto configPath = writeConfig();
    const auto logFile    = dir / "syslog.log";
    {
        std::ofstream f(logFile);
        f << "existing content\n";
    }

    const auto report = preflight(configPath, ServiceState::NotInstalled);
    BOOST_TEST(allFindings(report) == "");

    std::ifstream f(logFile);
    std::string content;
    std::getline(f, content);
    BOOST_TEST(content == "existing content");
    BOOST_TEST(fs::file_size(logFile) == 17U);
}

BOOST_AUTO_TEST_CASE(a_log_path_whose_parent_cannot_be_searched_is_not_called_missing)
{
    // A path that cannot be reached is not a path that is absent: one needs an
    // ACL fixed, the other needs a directory created. Only a POSIX case here —
    // there is no portable way to make a directory unsearchable.
#ifndef _WIN32
    if (!canDenyAccess())
    {
        return;
    }

    const auto parent = dir / "sealed";
    const auto logDir = parent / "logs";
    fs::create_directories(logDir);
    Options opts;
    opts.logDir           = logDir.string();
    const auto configPath = writeConfig(opts);
    fs::permissions(parent, fs::perms::none);

    const auto report = preflight(configPath, ServiceState::NotInstalled);
    // Restored here as well as in the fixture, so the rest of the case can read
    // the tree even if an assertion below aborts it.
    fs::permissions(parent, fs::perms::owner_all);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "cannot inspect log directory"));
    BOOST_TEST(!mentions(report, Finding::Level::Error, "does not exist"));
#endif
}

#ifndef _WIN32

BOOST_AUTO_TEST_CASE(an_unwritable_log_directory_says_it_is_not_writable)
{
    if (!canDenyAccess())
    {
        return;
    }

    const auto logDir = dir / "readonly";
    fs::create_directories(logDir);
    Options opts;
    opts.logDir = logDir.string();
    // Written before the bits are dropped: the config lives in the fixture
    // directory, but the log directory has to be unwritable by the time
    // preflight looks at it.
    const auto configPath = writeConfig(opts);
    fs::permissions(logDir, fs::perms::owner_read | fs::perms::owner_exec);

    const auto report = preflight(configPath, ServiceState::NotInstalled);
    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "exists but is not writable"));
}

// The dead-sink case that motivates --check: a writable directory is not enough
// if yesterday's log file has had its own permissions tightened.
BOOST_AUTO_TEST_CASE(an_existing_log_file_that_cannot_be_appended_to_is_an_error)
{
    const auto configPath = writeConfig();
    const auto logFile    = dir / "syslog.log";
    {
        std::ofstream f(logFile);
        f << "existing content\n";
    }
    fs::permissions(logFile, fs::perms::owner_read);

    if (!canDenyAccess())
    {
        return; // root appends to anything, so there is nothing to observe.
    }

    const auto report = preflight(configPath, ServiceState::NotInstalled);
    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "cannot be opened for appending"));
}

#endif // !_WIN32

BOOST_AUTO_TEST_SUITE_END()

// ─── Warnings that are not failures ──────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(warnings, Fixture)

// Rotation off is a deliberate setting (#16), so it is worth saying and is not
// a failure. A --check that called it an error would train people to ignore it.
BOOST_AUTO_TEST_CASE(rotation_disabled_is_a_warning_and_exits_zero)
{
    Options opts;
    opts.maxSize      = "0";
    const auto report = preflight(writeConfig(opts), ServiceState::NotInstalled);

    BOOST_TEST(mentions(report, Finding::Level::Warning, "max_size = 0"));
    BOOST_TEST(report.count(Finding::Level::Error) == 0U);
    BOOST_TEST(!report.failed());

    std::ostringstream out;
    BOOST_TEST(printPreflight(report, "cfg", out) == EXIT_SUCCESS);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── The listen port ─────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(listen_port, Fixture)

// Most people run --check on a machine where minilog is already installed and
// running. Its own socket holding the port is the healthy state, and reporting
// it as a fault would make --check useless exactly where it is most used.
BOOST_AUTO_TEST_CASE(a_port_held_while_the_service_runs_is_a_warning)
{
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket holder(
        ioc, {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(0)});

    Options opts;
    opts.udpPort      = holder.local_endpoint().port();
    const auto report = preflight(writeConfig(opts), ServiceState::Running);

    BOOST_TEST(mentions(report, Finding::Level::Warning, "held by the minilog service"));
    BOOST_TEST(!report.failed());
}

BOOST_AUTO_TEST_CASE(the_same_port_held_with_no_service_registered_is_an_error)
{
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket holder(
        ioc, {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(0)});

    Options opts;
    opts.udpPort      = holder.local_endpoint().port();
    const auto report = preflight(writeConfig(opts), ServiceState::NotInstalled);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "cannot bind UDP 127.0.0.1:"));
}

// A registered-but-stopped service cannot be the holder, so the port really is
// taken by something else and the report must not hedge.
BOOST_AUTO_TEST_CASE(a_stopped_service_does_not_excuse_a_taken_port)
{
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket holder(
        ioc, {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(0)});

    Options opts;
    opts.udpPort      = holder.local_endpoint().port();
    const auto report = preflight(writeConfig(opts), ServiceState::NotRunning);

    BOOST_TEST(report.failed());
    BOOST_TEST(!mentions(report, Finding::Level::Error, "service manager could not be asked"));
}

// The state every Linux run really uses. It stays an error — nothing there can
// attribute the socket to minilog, and a check that assumed it could would pass
// a machine with a genuine port clash — but it says what it could not check.
BOOST_AUTO_TEST_CASE(a_platform_with_no_service_manager_says_so_and_still_fails)
{
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket holder(
        ioc, {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(0)});

    Options opts;
    opts.udpPort      = holder.local_endpoint().port();
    const auto report = preflight(writeConfig(opts), ServiceState::NotApplicable);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "cannot bind UDP 127.0.0.1:"));
    BOOST_TEST(mentions(report, Finding::Level::Error, "this platform has no service manager"));
    BOOST_TEST(!mentions(report, Finding::Level::Error, "service manager could not be asked"));
}

BOOST_AUTO_TEST_CASE(an_unanswerable_service_manager_is_admitted_to_in_the_message)
{
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket holder(
        ioc, {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(0)});

    Options opts;
    opts.udpPort      = holder.local_endpoint().port();
    const auto report = preflight(writeConfig(opts), ServiceState::Unknown);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "service manager could not be asked"));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Forwarding ──────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(forwarding, Fixture)

BOOST_AUTO_TEST_CASE(a_resolvable_destination_becomes_a_requirement)
{
    Options opts;
    opts.forwardHost  = "127.0.0.1";
    const auto report = preflight(writeConfig(opts), ServiceState::NotInstalled);

    BOOST_TEST(asksFor(report, "outbound UDP to 127.0.0.1:1514 (forwarding)"));
    BOOST_TEST(allFindings(report) == "");
}

// The server deliberately keeps running with an unresolvable destination and
// retries in the background, which is why this failure is otherwise invisible:
// nothing is forwarded and nothing has failed.
BOOST_AUTO_TEST_CASE(an_unresolvable_destination_is_an_error_that_explains_itself)
{
    Options opts;
    // .invalid is reserved by RFC 2606 precisely so that it never resolves.
    opts.forwardHost  = "minilog-no-such-host.invalid";
    const auto report = preflight(writeConfig(opts), ServiceState::NotInstalled);

    BOOST_TEST(report.failed());
    BOOST_TEST(mentions(report, Finding::Level::Error, "does not resolve"));
    BOOST_TEST(mentions(report, Finding::Level::Error, "retries the lookup in the background"));
}

BOOST_AUTO_TEST_CASE(forwarding_left_disabled_is_not_resolved_at_all)
{
    // No [forwarding] section, so an unresolvable host cannot be reported: with
    // enabled = false the destination is not used, and validating it would fail
    // a machine over a setting that does nothing.
    const auto report = preflight(writeConfig(), ServiceState::NotInstalled);
    BOOST_TEST(!asksFor(report, "forwarding"));
    BOOST_TEST(allFindings(report) == "");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Printing ────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(output, Fixture)

BOOST_AUTO_TEST_CASE(a_clean_report_says_so_and_exits_zero)
{
    const auto configPath = writeConfig();
    std::ostringstream out;
    const int code =
        printPreflight(preflight(configPath, ServiceState::NotInstalled), configPath, out);

    BOOST_TEST(code == EXIT_SUCCESS);
    BOOST_TEST(out.str().find("No problems found") != std::string::npos);
    BOOST_TEST(out.str().find(configPath) != std::string::npos);
    // Handed to whoever provisions the firewall, so it is printed even when
    // there is nothing wrong.
    BOOST_TEST(out.str().find("This configuration requires:") != std::string::npos);
}

// No local test can show that a remote sender reaches the port, so the output
// has to say that rather than let a clean report imply it.
BOOST_AUTO_TEST_CASE(the_output_disclaims_what_it_cannot_check)
{
    std::ostringstream out;
    printPreflight(preflight(writeConfig(), ServiceState::NotInstalled), "cfg", out);
    BOOST_TEST(out.str().find("firewall rules") != std::string::npos);
}

// Every problem is reported in one run: a preflight that stopped at the first
// failure would force the fix-rerun cycle it exists to prevent.
BOOST_AUTO_TEST_CASE(errors_and_warnings_are_all_printed_and_counted)
{
    PreflightReport report;
    report.findings.push_back({Finding::Level::Error, "first problem"});
    report.findings.push_back({Finding::Level::Warning, "a caveat"});
    report.findings.push_back({Finding::Level::Error, "second problem"});

    std::ostringstream out;
    BOOST_TEST(printPreflight(report, "cfg", out) == EXIT_FAILURE);

    const auto text = out.str();
    BOOST_TEST(text.find("ERROR:   first problem") != std::string::npos);
    BOOST_TEST(text.find("WARNING: a caveat") != std::string::npos);
    BOOST_TEST(text.find("ERROR:   second problem") != std::string::npos);
    BOOST_TEST(text.find("2 error(s), 1 warning(s)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(several_independent_faults_are_all_found_in_one_run)
{
    Options opts;
    opts.logDir      = (dir / "not-provisioned").string();
    opts.maxSize     = "0";
    opts.forwardHost = "minilog-no-such-host.invalid";
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket holder(
        ioc, {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(0)});
    opts.udpPort = holder.local_endpoint().port();

    const auto report = preflight(writeConfig(opts), ServiceState::NotInstalled);
    BOOST_TEST(report.count(Finding::Level::Error) == 3U);
    BOOST_TEST(report.count(Finding::Level::Warning) == 1U);
}

BOOST_AUTO_TEST_SUITE_END()
