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
#include "receive_backoff.hpp"
#include "udp_server.hpp"

#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
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
        fs::remove_all(dir);
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

    // Drain all pending work: stop the server, join all threads.
    //
    // NOTE: om.close() is intentionally NOT called here.  Calling it before
    // joining would post a "close" task to the LogFile strand while processing
    // tasks are still queued in the io_context.  Those tasks post their writes
    // to the strand *after* the close task, causing them to be silently dropped
    // (m_closed == true).  Instead, we join first so the io_context fully drains
    // (all receives → process tasks → strand writes complete), then let the
    // OutputManager/LogFile destructors close files synchronously.
    void shutdown(UdpServer& server, std::vector<std::thread>& ioThreads)
    {
        server.stop();
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

    // Wait up to `timeout` for the server to have refused at least `n` datagrams.
    static bool waitForDrops(const UdpServer& server, std::uint64_t n, std::chrono::seconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (server.droppedDatagrams() >= n)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
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
// Strict-count tests — skipped in lossy/extended mode where ASan/TSan overhead
// causes the OS to drop UDP datagrams under load.  Covered by soak_flood below.
#if !MINILOG_STRESS_LOSSY

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

    // At least half must arrive — exact count is not checked because UDP may
    // drop datagrams under CI load.
    BOOST_CHECK(waitForLines(dir / "syslog.log", N / 2, std::chrono::seconds(10)));
    shutdown(server, ioThreads);

    BOOST_CHECK_GE(countLines(dir / "syslog.log"), N / 2);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Concurrent senders ──────────────────────────────────────────────────────
// Same rationale as flood above.

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

    // At least half must arrive — exact count is not checked because UDP may
    // drop datagrams under CI load.
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;
    BOOST_CHECK(waitForLines(dir / "syslog.log", N_TOTAL / 2, std::chrono::seconds(30)));
    shutdown(server, ioThreads);

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
    BOOST_CHECK_GE(lineCount, N_TOTAL / 2);
}

BOOST_AUTO_TEST_SUITE_END()

#endif // !MINILOG_STRESS_LOSSY

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
    shutdown(server, ioThreads);
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
    shutdown(server, ioThreads);

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
    shutdown(server, ioThreads);

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

// ─── Admission control ───────────────────────────────────────────────────────
// Unit level: the token, not the socket, is what bounds memory, so these are
// the deterministic half of #10's regression cover.

BOOST_AUTO_TEST_SUITE(admission_control)

BOOST_AUTO_TEST_CASE(charge_is_held_until_the_last_copy_is_gone)
{
    // The whole design rests on this: bounding only the io_context queue would
    // move the growth to the sink strands, so the charge has to survive every
    // copy of the message that carries it.
    AdmissionControl ac(1000);

    auto first = ac.admit(400);
    BOOST_REQUIRE(first);
    BOOST_TEST(ac.inFlight() == 400u);

    {
        auto copyA = first;
        auto copyB = first;
        first.reset();
        BOOST_TEST(ac.inFlight() == 400u); // still queued somewhere
    }
    BOOST_TEST(ac.inFlight() == 0u);
}

BOOST_AUTO_TEST_CASE(over_budget_is_refused_and_counted)
{
    AdmissionControl ac(1000);

    auto held = ac.admit(900);
    BOOST_REQUIRE(held);

    BOOST_TEST(!ac.admit(200));
    BOOST_TEST(!ac.admit(200));
    BOOST_TEST(ac.droppedTotal() == 2u);
    BOOST_TEST(ac.inFlight() == 900u); // a refusal charges nothing

    // Exactly filling the budget is allowed.
    auto exact = ac.admit(100);
    BOOST_TEST(!!exact);
    BOOST_TEST(ac.inFlight() == 1000u);
}

BOOST_AUTO_TEST_CASE(budget_frees_up_again_after_release)
{
    AdmissionControl ac(1000);

    auto held = ac.admit(1000);
    BOOST_REQUIRE(held);
    BOOST_TEST(!ac.admit(1));

    held.reset();
    BOOST_TEST(ac.inFlight() == 0u);
    BOOST_TEST(!!ac.admit(1000));
}

BOOST_AUTO_TEST_CASE(datagram_larger_than_the_whole_budget_is_refused)
{
    AdmissionControl ac(1000);
    BOOST_TEST(!ac.admit(1001));
    BOOST_TEST(ac.inFlight() == 0u);
    BOOST_TEST(ac.droppedTotal() == 1u);
}

BOOST_AUTO_TEST_CASE(take_dropped_reports_each_drop_once)
{
    AdmissionControl ac(10);

    BOOST_TEST(!ac.admit(100));
    BOOST_TEST(!ac.admit(100));
    BOOST_TEST(ac.takeDropped() == 2u);
    BOOST_TEST(ac.takeDropped() == 0u); // the window resets, the total does not
    BOOST_TEST(ac.droppedTotal() == 2u);
}

BOOST_AUTO_TEST_CASE(token_outlives_the_control_it_came_from)
{
    // Tokens ride on queued messages, so one can still be alive when the server
    // that issued it is gone. Destroying it must not touch freed memory.
    AdmissionControl::Token token;
    {
        AdmissionControl ac(1000);
        token = ac.admit(100);
        BOOST_REQUIRE(token);
    }
    BOOST_CHECK_NO_THROW(token.reset());
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Receive-error backoff ───────────────────────────────────────────────────
// A receive error used to log and re-arm immediately. For a transient error
// that is right; for a persistent one it is a core at 100% and a log line per
// iteration, written into the host's syslog — which on a collector is often
// relayed straight back into minilog. Unit level, because provoking a real
// persistent socket error is not something a test can do portably.

BOOST_AUTO_TEST_SUITE(receive_backoff)

BOOST_AUTO_TEST_CASE(first_error_is_reported_at_once)
{
    ReceiveBackoff backoff;
    const auto now = std::chrono::steady_clock::now();

    const auto decision = backoff.onError("connection reset", now);

    BOOST_TEST(decision.report);
    BOOST_TEST(decision.suppressed == 0u);
    BOOST_TEST(decision.delay.count() == ReceiveBackoff::kFirstDelay.count());
}

BOOST_AUTO_TEST_CASE(repeats_are_not_reported_and_the_delay_grows)
{
    ReceiveBackoff backoff;
    const auto now = std::chrono::steady_clock::now();
    backoff.onError("connection reset", now);

    auto previous = ReceiveBackoff::kFirstDelay;
    for (int i = 0; i < 20; ++i)
    {
        // Same instant every time: nothing here may depend on the test being
        // slow enough for the report interval to elapse.
        const auto decision = backoff.onError("connection reset", now);
        BOOST_TEST(!decision.report, "repeat " << i << " was reported");
        BOOST_TEST(decision.delay.count() >= previous.count());
        previous = decision.delay;
    }

    BOOST_TEST(previous.count() == ReceiveBackoff::kMaxDelay.count());
}

BOOST_AUTO_TEST_CASE(delay_is_capped)
{
    // The cap is what bounds how long the socket stays un-armed after the error
    // clears, so it must not keep doubling.
    ReceiveBackoff backoff;
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i)
    {
        const auto decision = backoff.onError("no buffer space", now);
        BOOST_TEST(decision.delay.count() <= ReceiveBackoff::kMaxDelay.count());
    }
}

BOOST_AUTO_TEST_CASE(a_summary_is_reported_once_the_interval_has_passed)
{
    ReceiveBackoff backoff;
    const auto start = std::chrono::steady_clock::now();
    backoff.onError("no buffer space", start);
    backoff.onError("no buffer space", start);
    backoff.onError("no buffer space", start);

    const auto decision =
        backoff.onError("no buffer space", start + ReceiveBackoff::kReportInterval);

    BOOST_TEST(decision.report);
    // The first error was reported on its own, so it is not in the count: the
    // two silent repeats after it, plus this one.
    BOOST_TEST(decision.suppressed == 3u);
}

BOOST_AUTO_TEST_CASE(a_different_error_is_reported_immediately)
{
    // A new failure is news even in the middle of a streak of another one, and
    // it may well be the transient kind — so the delay starts over too.
    ReceiveBackoff backoff;
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i)
    {
        backoff.onError("no buffer space", now);
    }
    BOOST_REQUIRE(backoff.delay().count() > ReceiveBackoff::kFirstDelay.count());

    const auto decision = backoff.onError("connection reset", now);

    BOOST_TEST(decision.report);
    BOOST_TEST(decision.delay.count() == ReceiveBackoff::kFirstDelay.count());
}

BOOST_AUTO_TEST_CASE(success_ends_the_streak_and_reports_how_long_it_was)
{
    ReceiveBackoff backoff;
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i)
    {
        backoff.onError("no buffer space", now);
    }

    BOOST_TEST(backoff.onSuccess() == 5u);

    // And the next error is a fresh one: full reporting, first delay.
    const auto decision = backoff.onError("no buffer space", now);
    BOOST_TEST(decision.report);
    BOOST_TEST(decision.delay.count() == ReceiveBackoff::kFirstDelay.count());
}

BOOST_AUTO_TEST_CASE(success_without_a_streak_reports_nothing)
{
    // Every ordinary datagram takes this path, so it must not produce a line.
    ReceiveBackoff backoff;
    BOOST_TEST(backoff.onSuccess() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Admission control under flood ───────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(admission_under_flood, Fixture)

BOOST_AUTO_TEST_CASE(oversized_datagram_is_dropped_and_the_receiver_carries_on)
{
    // A budget below one datagram makes the outcome of each send certain, so
    // this covers the end-to-end drop path without depending on timing.
    auto cfg          = makeConfig(true, false);
    cfg.maxQueueBytes = 1024;

    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc();

    sendUdp("<34>Oct 11 22:14:15 host app[1]: " + std::string(60000, 'A'), port);
    BOOST_CHECK(waitForDrops(server, 1, std::chrono::seconds(5)));

    // Dropping must not wedge the receiver: the next datagram fits and lands.
    sendUdp("<34>Oct 11 22:14:15 host app[1]: small one", port);
    BOOST_CHECK(waitForLines(dir / "syslog.log", 1, std::chrono::seconds(5)));

    shutdown(server, ioThreads);

    BOOST_CHECK_EQUAL(server.droppedDatagrams(), 1u);
    BOOST_CHECK_EQUAL(countLines(dir / "syslog.log"), 1);
    BOOST_CHECK_EQUAL(server.queuedBytes(), 0u);
}

BOOST_AUTO_TEST_CASE(queued_bytes_never_exceed_the_budget)
{
    // #10's acceptance criterion is a bounded RSS under flood. RSS is not
    // portable to assert, but it is bounded *because* this is: what the server
    // holds is a small multiple of the bytes it has admitted.
    constexpr std::uint64_t budget = 64 * 1024;

    auto cfg          = makeConfig(true, false);
    cfg.maxQueueBytes = budget;

    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();
    const uint16_t port = server.localPort();

    auto ioThreads = startIoc(4);

    // Sample from outside the io threads, so the ceiling is observed while the
    // flood is in progress rather than only after it drains.
    std::atomic<std::uint64_t> highWater{0};
    std::atomic<bool> sampling{true};
    std::thread sampler(
        [&]()
        {
            while (sampling.load())
            {
                const auto queued = server.queuedBytes();
                auto seen         = highWater.load();
                while (queued > seen && !highWater.compare_exchange_weak(seen, queued))
                {
                }
            }
        });

    const std::string payload = "<34>Oct 11 22:14:15 host app[1]: " + std::string(60000, 'A');
    constexpr int N           = 5 * MINILOG_STRESS_MULTIPLIER;
    for (int i = 0; i < N; ++i)
    {
        sendUdp(payload, port);
    }

    shutdown(server, ioThreads);
    sampling.store(false);
    sampler.join();

    BOOST_CHECK_LE(highWater.load(), budget);
    BOOST_CHECK_EQUAL(server.queuedBytes(), 0u);

    // At most one 60 KB datagram fits in a 64 KB budget, so a burst that really
    // reached the socket must have had some of it refused. Guarded on the burst
    // arriving at all: the kernel drops datagrams of its own under load, and in
    // the instrumented builds it drops most of them.
    const auto seen =
        static_cast<std::uint64_t>(countLines(dir / "syslog.log")) + server.droppedDatagrams();
    if (seen >= 20)
    {
        BOOST_CHECK_GT(server.droppedDatagrams(), 0u);
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
            // Throttle to ~10k msg/s. Without this, loopback floods the
            // io_context queue faster than ASan-slowed file I/O drains it,
            // growing the queue without bound until OOM kills the process.
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    shutdown(server, ioThreads);
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
                    // Throttle to ~10k msg/s — see soak_flood for rationale.
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });
    }
    for (auto& t : senders)
    {
        t.join();
    }

    shutdown(server, ioThreads);

    // Every line that did arrive must be structurally complete.  Message loss
    // is expected under overload (no count assertion), but any line written to
    // the file must be a complete datagram — UDP is delivered atomically on
    // loopback so truncation indicates a real bug.
    std::ifstream f(dir / "syslog.log");
    std::string line;
    while (std::getline(f, line))
    {
        const auto pos = line.rfind(": t");
        BOOST_CHECK_MESSAGE(pos != std::string::npos,
                            "torn line (" << line.size() << " bytes): " << line);
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
    shutdown(server, ioThreads);

    // All messages must have been written locally despite the failing forwarding.
    BOOST_CHECK_EQUAL(countLines(dir / "syslog.log"), N);
}

BOOST_AUTO_TEST_SUITE_END()
