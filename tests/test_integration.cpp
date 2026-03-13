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

#define BOOST_TEST_MODULE test_integration
#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"
#include "server/udp_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace minilog;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace
{

struct Fixture
{
    fs::path dir;
    boost::asio::io_context ioc;

    Fixture()
    {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("minilog_itest_" + std::to_string(++counter));
        fs::create_directories(dir);
    }

    ~Fixture() { fs::remove_all(dir); }

    Config makeConfig(bool textOut = true, bool jsonlOut = true, bool inclMalformed = true) const
    {
        Config cfg;
        cfg.host    = "127.0.0.1";
        cfg.udpPort = 0; // OS assigns an ephemeral port

        OutputConfig out;
        out.name             = "main";
        out.includeMalformed = inclMalformed;
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

    // Run the ioc long enough to receive and fully process all sent messages,
    // then stop the server and drain remaining strand work.
    void drain(UdpServer& server, OutputManager& om)
    {
        ioc.run_for(std::chrono::milliseconds(100));
        server.stop();
        om.close();
        ioc.restart();
        ioc.run_for(std::chrono::milliseconds(50));
    }

    std::string readAll(const fs::path& p) const
    {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }

    bj::object parseJsonl(const std::string& content) const
    {
        return bj::parse(std::string(content.begin(), content.end() - 1)).as_object();
    }
};

} // namespace

// ─── RFC3164 pipeline ────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(rfc3164_pipeline, Fixture)

BOOST_AUTO_TEST_CASE(text_file_contains_raw_payload)
{
    auto cfg = makeConfig(/*textOut=*/true, /*jsonlOut=*/false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    const std::string msg = "<34>Oct 11 22:14:15 mymachine su[123]: hello world";
    sendUdp(msg, server.localPort());
    drain(server, om);

    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), msg + "\n");
}

BOOST_AUTO_TEST_CASE(jsonl_fields_correct)
{
    auto cfg = makeConfig(/*textOut=*/false, /*jsonlOut=*/true);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    sendUdp("<34>Oct 11 22:14:15 mymachine su[123]: hello world", server.localPort());
    drain(server, om);

    auto obj = parseJsonl(readAll(dir / "syslog.jsonl"));

    BOOST_CHECK_EQUAL(obj["proto"].as_string(), "RFC3164");
    BOOST_CHECK_EQUAL(obj["src"].as_string(), "127.0.0.1");
    BOOST_CHECK_EQUAL(obj["hostname"].as_string(), "mymachine");
    BOOST_CHECK_EQUAL(obj["app"].as_string(), "su");
    BOOST_CHECK_EQUAL(obj["pid"].as_string(), "123");
    BOOST_CHECK_EQUAL(obj["message"].as_string(), "hello world");
    BOOST_CHECK(obj["msgid"].is_null());
}

BOOST_AUTO_TEST_CASE(jsonl_rcv_is_iso8601_utc)
{
    auto cfg = makeConfig(false, true);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    sendUdp("<34>Oct 11 22:14:15 mymachine su[123]: test", server.localPort());
    drain(server, om);

    const auto rcv = std::string(parseJsonl(readAll(dir / "syslog.jsonl"))["rcv"].as_string());

    // YYYY-MM-DDTHH:MM:SSZ
    BOOST_REQUIRE_EQUAL(rcv.size(), 20u);
    BOOST_CHECK_EQUAL(rcv[4], '-');
    BOOST_CHECK_EQUAL(rcv[7], '-');
    BOOST_CHECK_EQUAL(rcv[10], 'T');
    BOOST_CHECK_EQUAL(rcv[13], ':');
    BOOST_CHECK_EQUAL(rcv[16], ':');
    BOOST_CHECK_EQUAL(rcv[19], 'Z');
}

BOOST_AUTO_TEST_CASE(jsonl_src_is_sender_ip)
{
    auto cfg = makeConfig(false, true);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    sendUdp("<34>Oct 11 22:14:15 mymachine su[123]: test", server.localPort());
    drain(server, om);

    BOOST_CHECK_EQUAL(parseJsonl(readAll(dir / "syslog.jsonl"))["src"].as_string(), "127.0.0.1");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── RFC5424 pipeline ────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(rfc5424_pipeline, Fixture)

BOOST_AUTO_TEST_CASE(text_file_contains_raw_payload)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    const std::string msg = "<34>1 2026-03-12T14:30:22Z mymachine su 123 ID47 - hello";
    sendUdp(msg, server.localPort());
    drain(server, om);

    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), msg + "\n");
}

BOOST_AUTO_TEST_CASE(jsonl_fields_correct)
{
    auto cfg = makeConfig(false, true);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    sendUdp("<34>1 2026-03-12T14:30:22Z mymachine su 123 ID47 - hello world", server.localPort());
    drain(server, om);

    auto obj = parseJsonl(readAll(dir / "syslog.jsonl"));

    BOOST_CHECK_EQUAL(obj["proto"].as_string(), "RFC5424");
    BOOST_CHECK_EQUAL(obj["hostname"].as_string(), "mymachine");
    BOOST_CHECK_EQUAL(obj["app"].as_string(), "su");
    BOOST_CHECK_EQUAL(obj["pid"].as_string(), "123");
    BOOST_CHECK_EQUAL(obj["msgid"].as_string(), "ID47");
    // Parser keeps STRUCTURED-DATA verbatim in message (status quo).
    // Nil SD "-" is included as a prefix before the MSG text.
    BOOST_CHECK_EQUAL(obj["message"].as_string(), "- hello world");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Mixed session ───────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(mixed_session, Fixture)

BOOST_AUTO_TEST_CASE(rfc3164_and_rfc5424_both_written)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    const uint16_t port = server.localPort();
    sendUdp("<34>Oct 11 22:14:15 mymachine su[123]: from3164", port);
    sendUdp("<34>1 2026-03-12T14:30:22Z mymachine su 123 - - from5424", port);
    drain(server, om);

    const std::string content = readAll(dir / "syslog.log");
    BOOST_CHECK(content.find("from3164") != std::string::npos);
    BOOST_CHECK(content.find("from5424") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Malformed messages ──────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(malformed_messages, Fixture)

BOOST_AUTO_TEST_CASE(include_malformed_true_writes_unknown)
{
    auto cfg = makeConfig(true, false, /*inclMalformed=*/true);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    sendUdp("not a syslog message at all", server.localPort());
    drain(server, om);

    BOOST_CHECK(!readAll(dir / "syslog.log").empty());
}

BOOST_AUTO_TEST_CASE(include_malformed_false_drops_unknown)
{
    auto cfg = makeConfig(true, false, /*inclMalformed=*/false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    sendUdp("not a syslog message at all", server.localPort());
    drain(server, om);

    BOOST_CHECK(!fs::exists(dir / "syslog.log"));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Graceful shutdown (kept below forwarding) ───────────────────────────────

BOOST_FIXTURE_TEST_SUITE(graceful_shutdown, Fixture)

BOOST_AUTO_TEST_CASE(all_messages_written_before_stop)
{
    auto cfg = makeConfig(true, false);
    OutputManager om(ioc, cfg);
    UdpServer server(ioc, cfg, om, nullptr);
    server.start();

    const uint16_t port = server.localPort();
    for (int i = 0; i < 5; ++i)
    {
        sendUdp("<34>Oct 11 22:14:15 mymachine su[123]: msg" + std::to_string(i), port);
    }
    drain(server, om);

    int lineCount = 0;
    for (char c : readAll(dir / "syslog.log"))
    {
        if (c == '\n')
        {
            ++lineCount;
        }
    }
    BOOST_CHECK_EQUAL(lineCount, 5);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Forwarding integration ───────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(forwarding_integration, Fixture)

BOOST_AUTO_TEST_CASE(forward_to_self)
{
    // Server A: text output in dir/a/, forwarding enabled (target = server B).
    // Server B: text output in dir/b/, no forwarding.
    // Send one message to A; both A and B output files must contain it.

    const fs::path dirA = dir / "a";
    const fs::path dirB = dir / "b";
    fs::create_directories(dirA);
    fs::create_directories(dirB);

    // Build config for B first (no forwarding) so we get its ephemeral port.
    Config cfgB;
    cfgB.host    = "127.0.0.1";
    cfgB.udpPort = 0;
    {
        OutputConfig out;
        out.name     = "b";
        out.textFile = (dirB / "syslog.log").string();
        cfgB.outputs = {out};
    }

    OutputManager omB(ioc, cfgB);
    UdpServer serverB(ioc, cfgB, omB, nullptr);
    serverB.start();

    const uint16_t portB = serverB.localPort();

    // Build config for A with forwarding pointed at B.
    Config cfgA;
    cfgA.host    = "127.0.0.1";
    cfgA.udpPort = 0;
    {
        OutputConfig out;
        out.name     = "a";
        out.textFile = (dirA / "syslog.log").string();
        cfgA.outputs = {out};
    }
    cfgA.forwarding.enabled = true;
    cfgA.forwarding.host    = "127.0.0.1";
    cfgA.forwarding.port    = portB;

    OutputManager omA(ioc, cfgA);
    Forwarder fwd(ioc, cfgA.forwarding);
    UdpServer serverA(ioc, cfgA, omA, &fwd);
    serverA.start();

    const std::string msg = "<34>Oct 11 22:14:15 mymachine su[123]: forwarded";
    sendUdp(msg, serverA.localPort());

    // Extra time for A->B forwarding leg to arrive and be processed.
    ioc.run_for(std::chrono::milliseconds(200));
    serverA.stop();
    serverB.stop();
    omA.close();
    omB.close();
    ioc.restart();
    ioc.run_for(std::chrono::milliseconds(50));

    BOOST_CHECK(readAll(dirA / "syslog.log").find("forwarded") != std::string::npos);
    BOOST_CHECK(readAll(dirB / "syslog.log").find("forwarded") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Port-busy error handling ─────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(port_busy, Fixture)

BOOST_AUTO_TEST_CASE(start_throws_when_port_is_in_use)
{
    // Start a first server to occupy the port.
    auto cfg1 = makeConfig(false, false);
    OutputManager om1(ioc, cfg1);
    UdpServer server1(ioc, cfg1, om1, nullptr);
    server1.start();
    const uint16_t busyPort = server1.localPort();

    // A second server on the same port must fail to start.
    auto cfg2    = makeConfig(false, false);
    cfg2.udpPort = busyPort;
    OutputManager om2(ioc, cfg2);
    UdpServer server2(ioc, cfg2, om2, nullptr);

    BOOST_CHECK_THROW(server2.start(), std::runtime_error);

    server1.stop();
    om1.close();
    ioc.restart();
    ioc.run_for(std::chrono::milliseconds(50));
}

BOOST_AUTO_TEST_CASE(start_exception_message_contains_port_number)
{
    auto cfg1 = makeConfig(false, false);
    OutputManager om1(ioc, cfg1);
    UdpServer server1(ioc, cfg1, om1, nullptr);
    server1.start();
    const uint16_t busyPort = server1.localPort();

    auto cfg2    = makeConfig(false, false);
    cfg2.udpPort = busyPort;
    OutputManager om2(ioc, cfg2);
    UdpServer server2(ioc, cfg2, om2, nullptr);

    try
    {
        server2.start();
        BOOST_FAIL("expected std::runtime_error");
    }
    catch (const std::runtime_error& e)
    {
        BOOST_CHECK(std::string(e.what()).find(std::to_string(busyPort)) != std::string::npos);
    }

    server1.stop();
    om1.close();
    ioc.restart();
    ioc.run_for(std::chrono::milliseconds(50));
}

BOOST_AUTO_TEST_SUITE_END()
