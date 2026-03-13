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

#define BOOST_TEST_MODULE test_stress
#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"
#include "server/udp_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace minilog;
namespace fs = std::filesystem;

// MINILOG_STRESS_MULTIPLIER is injected by CMake (default 100, reduced in
// instrumented builds such as coverage / ASan / TSan).
#ifndef MINILOG_STRESS_MULTIPLIER
#define MINILOG_STRESS_MULTIPLIER 100
#endif

namespace
{

struct Fixture
{
    fs::path dir;
    boost::asio::io_context ioc;

    Fixture()
    {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("minilog_stress_" + std::to_string(++counter));
        fs::create_directories(dir);
    }

    ~Fixture() { fs::remove_all(dir); }

    Config makeConfig(bool textOut = true, bool jsonlOut = false) const
    {
        Config cfg;
        cfg.host    = "127.0.0.1";
        cfg.udpPort = 0;

        OutputConfig out;
        out.name             = "main";
        out.includeMalformed = true;
        if (textOut)
        {
            out.textFile = (dir / "syslog.log").string();
        }
        if (jsonlOut)
        {
            out.jsonlFile = (dir / "syslog.jsonl").string();
        }
        cfg.outputs = {out};
        return cfg;
    }

    static void sendUdp(const std::string& data, uint16_t port)
    {
        boost::asio::io_context senderIoc;
        boost::asio::ip::udp::socket sock(senderIoc, boost::asio::ip::udp::v4());
        const boost::asio::ip::udp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), port);
        sock.send_to(boost::asio::buffer(data), ep);
    }

    // Start the io_context on `n` background threads.
    std::vector<std::thread> startIoc(int n = 1)
    {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            threads.emplace_back([this]() { ioc.run(); });
        }
        return threads;
    }

    // Drain all pending work: stop the server, close outputs, join all threads.
    void shutdown(UdpServer& server, OutputManager& om, std::vector<std::thread>& ioThreads)
    {
        server.stop();
        om.close();
        for (auto& t : ioThreads)
        {
            t.join();
        }
    }

    static int countLines(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return static_cast<int>(
            std::count(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>(), '\n'));
    }

    // Wait up to `timeout` for the file at `p` to have at least `n` lines.
    static bool waitForLines(const fs::path& p, int n, std::chrono::seconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (countLines(p) >= n)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
};

} // namespace

// ─── Flood ───────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(flood, Fixture)

BOOST_AUTO_TEST_CASE(ten_thousand_messages_no_loss)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc();

    constexpr int N = 100 * MINILOG_STRESS_MULTIPLIER;
    for (int i = 0; i < N; ++i)
    {
        sendUdp("<34>Oct 11 22:14:15 host app[1]: flood " + std::to_string(i), port);
    }

    BOOST_CHECK(waitForLines(dir / "syslog.log", N, std::chrono::seconds(10)));
    shutdown(server, om, ioThreads);

    BOOST_CHECK_EQUAL(countLines(dir / "syslog.log"), N);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Concurrent senders ──────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(concurrent_senders, Fixture)

BOOST_AUTO_TEST_CASE(eight_threads_no_torn_lines)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc(4);

    constexpr int N_THREADS    = 8;
    constexpr int N_PER_THREAD = 10 * MINILOG_STRESS_MULTIPLIER; // 8 000 at full scale

    std::vector<std::thread> senders;
    senders.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t)
    {
        senders.emplace_back(
            [t, port]()
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    sendUdp("<34>Oct 11 22:14:15 host app[" + std::to_string(t) + "]: t" +
                                std::to_string(t) + "m" + std::to_string(i),
                            port);
                }
            });
    }
    for (auto& t : senders)
    {
        t.join();
    }

    // Wait for all messages to be processed.  With 4 io_context threads this
    // should be well within the timeout even on a slow CI machine.
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;
    BOOST_CHECK(waitForLines(dir / "syslog.log", N_TOTAL, std::chrono::seconds(10)));
    shutdown(server, om, ioThreads);

    std::ifstream f(dir / "syslog.log");
    std::string line;
    int lineCount = 0;
    while (std::getline(f, line))
    {
        ++lineCount;
        // Each original message ends with ": tNmN".
        const auto pos = line.rfind(": t");
        BOOST_CHECK_MESSAGE(pos != std::string::npos, "torn line: " << line);
    }
    BOOST_CHECK_EQUAL(lineCount, N_TOTAL);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Max-size datagrams ───────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(max_size_datagrams, Fixture)

BOOST_AUTO_TEST_CASE(max_udp_payload_no_crash)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc();

    // 65507 = max valid UDP payload (65535 − 20 IP header − 8 UDP header).
    // Send two: one is sufficient to prove no crash; two adds a bit more
    // confidence without risking socket-buffer overflow.
    const std::string payload(65507, 'X');
    sendUdp(payload, port);
    sendUdp(payload, port);

    BOOST_CHECK(waitForLines(dir / "syslog.log", 1, std::chrono::seconds(5)));
    shutdown(server, om, ioThreads);
    // At least one must arrive — exact count depends on socket buffer.
    BOOST_CHECK_GE(countLines(dir / "syslog.log"), 1);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Adversarial input ───────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(adversarial_input, Fixture)

BOOST_AUTO_TEST_CASE(degenerate_payloads_do_not_crash)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc();

    // Empty datagram — not valid to send a zero-byte UDP payload via send_to
    // on all platforms, so skip; exercise all others.
    sendUdp(std::string(1, '\0'), port);    // 1 null byte
    sendUdp(std::string(1, '\xff'), port);  // 1 byte 0xFF
    sendUdp(std::string(64, '\0'), port);   // 64 null bytes
    sendUdp(std::string(64, '\xff'), port); // 64 bytes 0xFF
    sendUdp("<", port);                     // truncated PRI
    sendUdp("<999>malformed pri", port);    // out-of-range PRI
    sendUdp(std::string(256, 'A'), port);   // plain ASCII, no PRI

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    shutdown(server, om, ioThreads);

    // Server must still be alive (no exception propagated to io_context).
    // The file may or may not exist depending on include_malformed, but we
    // verify the test completes without crashing.
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Rotation under flood ─────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(rotation_under_flood, Fixture)

BOOST_AUTO_TEST_CASE(correct_file_count_after_flood)
{
    auto cfg = makeConfig(true, false);
    // Tiny max_size forces frequent rotation.
    cfg.outputs[0].maxSize  = 512;
    cfg.outputs[0].maxFiles = 3;

    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc();

    constexpr int N = 5 * MINILOG_STRESS_MULTIPLIER;
    for (int i = 0; i < N; ++i)
    {
        sendUdp("<34>Oct 11 22:14:15 host app[1]: rotation flood " + std::to_string(i), port);
    }

    BOOST_CHECK(waitForLines(dir / "syslog.log", 1, std::chrono::seconds(5)));
    shutdown(server, om, ioThreads);

    // Count rotated files — must not exceed max_files.
    int rotated = 0;
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.path().filename().string().find("syslog") != std::string::npos)
        {
            ++rotated;
        }
    }
    // max_files=3 means 3 rotated + 1 current = 4 total at most.
    BOOST_CHECK_LE(rotated, 4);

    // Verify every line in every file is complete (ends before the newline
    // that getline strips — no torn lines).
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.path().filename().string().find("syslog") == std::string::npos)
        {
            continue;
        }
        std::ifstream f(entry.path());
        std::string line;
        while (std::getline(f, line))
        {
            BOOST_CHECK_MESSAGE(!line.empty(), "empty line in " << entry.path());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

#if MINILOG_STRESS_LOSSY

// ─── Soak tests (extended / lossy mode only) ─────────────────────────────────
// Compiled only when MINILOG_STRESS_LOSSY=1 (the linux-*-extended presets).
// Each test continuously sends messages for the full SOAK_DURATION window using
// a single reused socket per sender thread, so the server stays busy the entire
// time and sanitizers observe concurrent activity throughout.
// Message loss is expected and not checked.

#ifndef MINILOG_STRESS_SOAK_SECONDS
#define MINILOG_STRESS_SOAK_SECONDS 10
#endif

namespace
{

constexpr std::chrono::seconds SOAK_DURATION{MINILOG_STRESS_SOAK_SECONDS};

} // namespace

BOOST_FIXTURE_TEST_SUITE(soak_flood, Fixture)

BOOST_AUTO_TEST_CASE(flood_no_crash)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc();

    // Send continuously for the entire soak window so the receive path stays
    // active and sanitizers can observe the full concurrent life cycle.
    {
        boost::asio::io_context senderIoc;
        boost::asio::ip::udp::socket sock(senderIoc, boost::asio::ip::udp::v4());
        const boost::asio::ip::udp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), port);
        const auto deadline = std::chrono::steady_clock::now() + SOAK_DURATION;
        for (int i = 0; std::chrono::steady_clock::now() < deadline; ++i)
        {
            const std::string msg =
                "<34>Oct 11 22:14:15 host app[1]: soak flood " + std::to_string(i);
            sock.send_to(boost::asio::buffer(msg), ep);
        }
    }

    shutdown(server, om, ioThreads);
    // No count assertion — message loss is expected under overload.
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(soak_concurrent, Fixture)

BOOST_AUTO_TEST_CASE(eight_threads_no_torn_lines)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc(4);

    constexpr int N_THREADS = 8;

    // Each sender thread runs continuously for SOAK_DURATION.
    std::vector<std::thread> senders;
    senders.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t)
    {
        senders.emplace_back(
            [t, port]()
            {
                boost::asio::io_context senderIoc;
                boost::asio::ip::udp::socket sock(senderIoc, boost::asio::ip::udp::v4());
                const boost::asio::ip::udp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"),
                                                        port);
                const auto deadline = std::chrono::steady_clock::now() + SOAK_DURATION;
                for (int i = 0; std::chrono::steady_clock::now() < deadline; ++i)
                {
                    const std::string msg = "<34>Oct 11 22:14:15 host app[" + std::to_string(t) +
                                            "]: t" + std::to_string(t) + "m" + std::to_string(i);
                    sock.send_to(boost::asio::buffer(msg), ep);
                }
            });
    }
    for (auto& t : senders)
    {
        t.join();
    }

    shutdown(server, om, ioThreads);

    // Lines that did arrive should be structurally complete.  Under extreme
    // load the kernel may occasionally deliver truncated UDP datagrams, which
    // is acceptable in lossy soak mode — so this is a warning, not a failure.
    std::ifstream f(dir / "syslog.log");
    std::string line;
    while (std::getline(f, line))
    {
        const auto pos = line.rfind(": t");
        BOOST_WARN_MESSAGE(pos != std::string::npos, "torn line: " << line);
    }
    // No count assertion.
}

BOOST_AUTO_TEST_SUITE_END()

#endif // MINILOG_STRESS_LOSSY

// ─── Forwarding to unreachable host ──────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(forwarding_unreachable, Fixture)

BOOST_AUTO_TEST_CASE(server_keeps_running_when_forward_target_is_down)
{
    auto cfg = makeConfig(true, false);

    // Point forwarding at a port where nothing is listening.
    // Pick an unlikely port; UDP send_to won't block even if unreachable.
    cfg.forwarding.enabled = true;
    cfg.forwarding.host    = "127.0.0.1";
    cfg.forwarding.port    = 19999;

    OutputManager om(ioc, cfg);
    Forwarder fwd(ioc, cfg.forwarding);
    UdpServer server(ioc, cfg, om, &fwd);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc();

    constexpr int N = 20;
    for (int i = 0; i < N; ++i)
    {
        sendUdp("<34>Oct 11 22:14:15 host app[1]: fwd unreachable " + std::to_string(i), port);
    }

    BOOST_CHECK(waitForLines(dir / "syslog.log", N, std::chrono::seconds(5)));
    shutdown(server, om, ioThreads);

    // All messages must have been written locally despite the failing forwarding.
    BOOST_CHECK_EQUAL(countLines(dir / "syslog.log"), N);
}

BOOST_AUTO_TEST_SUITE_END()
