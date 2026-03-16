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

#define BOOST_TEST_MODULE test_forwarder
#include "forwarder/forwarder.hpp"
#include "parser/syslog_message.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace minilog;

namespace
{

// Opens a UDP socket on an ephemeral port so tests can receive forwarded packets.
struct Receiver
{
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket sock;

    Receiver() : sock(ioc, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0))
    {
        sock.non_blocking(true);
    }

    uint16_t port() const { return sock.local_endpoint().port(); }

    // Returns the next datagram payload, or empty string if nothing arrived.
    std::string receive()
    {
        std::vector<char> buf(65507);
        boost::system::error_code ec;
        const std::size_t n = sock.receive(boost::asio::buffer(buf), 0, ec);
        if (ec)
        {
            return {};
        }
        return std::string(buf.data(), n);
    }
};

ForwardingConfig makeConfig(uint16_t port,
                            bool enabled                = true,
                            std::vector<int> facilities = {},
                            uint32_t maxMsgSize         = 0)
{
    ForwardingConfig cfg;
    cfg.enabled        = enabled;
    cfg.host           = "127.0.0.1";
    cfg.port           = port;
    cfg.facilities     = std::move(facilities);
    cfg.maxMessageSize = maxMsgSize;
    return cfg;
}

SyslogMessage makeMsg(const std::string& raw, std::optional<int> facility = std::nullopt)
{
    SyslogMessage msg;
    msg.raw      = raw;
    msg.facility = facility;
    return msg;
}

// Run the io_context just long enough to flush posted strand work.
void drain(boost::asio::io_context& ioc)
{
    ioc.run_for(std::chrono::milliseconds(50));
    ioc.restart();
}

} // namespace

// ─── Forwarding enabled / disabled ───────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(enabled_disabled)

BOOST_AUTO_TEST_CASE(disabled_does_not_send)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), /*enabled=*/false));

    fwd.forward(makeMsg("hello"));
    drain(ioc);

    BOOST_CHECK(rx.receive().empty());
}

BOOST_AUTO_TEST_CASE(enabled_sends_raw_payload)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port()));

    fwd.forward(makeMsg("hello world"));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "hello world");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Facility filtering ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(facility_filtering)

BOOST_AUTO_TEST_CASE(empty_filter_forwards_all)
{
    Receiver rx;
    boost::asio::io_context ioc;
    // facilities={} → wildcard
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {}));

    fwd.forward(makeMsg("msg", 4));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "msg");
}

BOOST_AUTO_TEST_CASE(matching_facility_is_forwarded)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {4, 16}));

    fwd.forward(makeMsg("kern msg", 4));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "kern msg");
}

BOOST_AUTO_TEST_CASE(non_matching_facility_is_dropped)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {4}));

    fwd.forward(makeMsg("other msg", 16));
    drain(ioc);

    BOOST_CHECK(rx.receive().empty());
}

BOOST_AUTO_TEST_CASE(no_facility_on_message_dropped_by_non_wildcard_filter)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {4}));

    fwd.forward(makeMsg("malformed msg")); // facility = nullopt
    drain(ioc);

    BOOST_CHECK(rx.receive().empty());
}

BOOST_AUTO_TEST_CASE(no_facility_on_message_forwarded_by_wildcard_filter)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {})); // wildcard

    fwd.forward(makeMsg("malformed msg"));               // facility = nullopt
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "malformed msg");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Truncation ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(truncation)

BOOST_AUTO_TEST_CASE(no_truncation_when_maxSize_zero)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {}, /*maxMsgSize=*/0));

    const std::string longMsg(500, 'A');
    fwd.forward(makeMsg(longMsg));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), longMsg);
}

BOOST_AUTO_TEST_CASE(no_truncation_when_exactly_at_limit)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {}, /*maxMsgSize=*/10));

    fwd.forward(makeMsg("1234567890")); // exactly 10 bytes
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "1234567890");
}

BOOST_AUTO_TEST_CASE(truncated_message_fits_within_max_size)
{
    Receiver rx;
    boost::asio::io_context ioc;
    constexpr uint32_t limit = 50;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {}, limit));

    const std::string longMsg(200, 'X');
    fwd.forward(makeMsg(longMsg));
    drain(ioc);

    const std::string got = rx.receive();
    BOOST_REQUIRE(!got.empty());
    BOOST_CHECK_LE(got.size(), limit);
    BOOST_CHECK(got.find("[TRUNCATED:") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(truncated_message_suffix_contains_original_size)
{
    Receiver rx;
    boost::asio::io_context ioc;
    constexpr uint32_t limit = 50;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {}, limit));

    const std::string longMsg(200, 'X');
    fwd.forward(makeMsg(longMsg));
    drain(ioc);

    const std::string got = rx.receive();
    BOOST_REQUIRE(!got.empty());
    BOOST_CHECK(got.find("200") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(maxSize_smaller_than_suffix_truncates_to_maxSize)
{
    // When maxSize is smaller than the "[TRUNCATED: N bytes]" suffix itself,
    // the message is simply clipped to maxSize bytes with no suffix appended.
    Receiver rx;
    boost::asio::io_context ioc;
    constexpr uint32_t limit = 5;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {}, limit));

    const std::string longMsg(200, 'X');
    fwd.forward(makeMsg(longMsg));
    drain(ioc);

    const std::string got = rx.receive();
    BOOST_CHECK_EQUAL(got, std::string(5, 'X'));
}

BOOST_AUTO_TEST_SUITE_END()
