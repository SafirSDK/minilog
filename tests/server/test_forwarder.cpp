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

// Like Receiver, but bound to a given protocol and address, so a test can
// listen wherever the name it uses resolves to.
struct ReceiverOn
{
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket sock;

    explicit ReceiverOn(const boost::asio::ip::udp::endpoint& bindTo) : sock(ioc, bindTo)
    {
        sock.non_blocking(true);
    }

    uint16_t port() const { return sock.local_endpoint().port(); }

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

// Let the destination resolve.
//
// A name's lookup is started by the constructor but completes on the io_context
// (see #38), so a test that expects a datagram to arrive has to run the
// io_context before it forwards anything — otherwise the forward runs on the
// strand first and is dropped as unresolved, which is the documented cost of not
// blocking the UDP bind on a name lookup.
//
// For an IP literal there is nothing to wait for: the constructor has already
// adopted the endpoint, so this returns true on the first check without running
// anything. Tests using a literal keep the call anyway, so that what they assert
// does not depend on knowing which of the two the host string was.
bool waitResolved(boost::asio::io_context& ioc,
                  const Forwarder& fwd,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!fwd.resolved() && std::chrono::steady_clock::now() < deadline)
    {
        ioc.run_for(std::chrono::milliseconds(10));
        ioc.restart();
    }
    return fwd.resolved();
}

// Let n lookups come back, successful or not.
//
// A failed lookup has no outward sign — the failure goes to the system log and
// forwarding just stays off — so without this a test against an unresolvable
// name reaches its assertions before the lookup has returned, and passes without
// having exercised the failure path at all.
//
// The generous timeout is for the host this runs on rather than for the code. A
// name under .invalid is guaranteed not to resolve, not to resolve quickly:
// a resolver that forwards it upstream instead of answering NXDOMAIN itself
// takes its own timeout over it, which on a glibc host walking its resolv.conf
// attempts is around ten seconds per lookup — the same reason test_binary.py
// waits as long as it does. A test that waits for two attempts pays that twice
// plus kFirstRetryDelay, so 10 s was a timeout the CI runners could cross while
// the code was working correctly.
bool waitAttempts(boost::asio::io_context& ioc,
                  const Forwarder& fwd,
                  uint64_t n,
                  std::chrono::milliseconds timeout = std::chrono::seconds(60))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (fwd.resolveAttempts() < n && std::chrono::steady_clock::now() < deadline)
    {
        ioc.run_for(std::chrono::milliseconds(10));
        ioc.restart();
    }
    return fwd.resolveAttempts() >= n;
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
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("hello world"));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "hello world");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Destination resolution ───────────────────────────────────────────────────
//
// The destination may be an IP literal, which the constructor parses and adopts,
// or a name, whose lookup the constructor only starts — it completes on the
// io_context. A name that does not resolve is reported and retried in the
// background rather than failing the process, because a Windows auto-start
// service routinely runs before DNS does.

BOOST_AUTO_TEST_SUITE(destination_resolution)

BOOST_AUTO_TEST_CASE(an_ip_literal_is_ready_before_the_io_context_runs)
{
    // A literal is parsed, not looked up, so there is nothing to wait for and
    // nothing that can block the UDP bind either way. It used to go through the
    // resolver along with names, which read tidily and left a window between the
    // bind and the completion handler where datagrams to a reachable collector
    // were dropped: never on an idle host, about one start in fourteen under
    // load, and it took the whole first burst rather than a single datagram.
    //
    // Asserted without running the io_context at all, which is the only way to
    // say "ready before anything else has had a chance to run" — and is exactly
    // the state the server is in between constructing the Forwarder and binding
    // the socket.
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port()));

    BOOST_TEST(fwd.resolved());
    // No lookup was started, so a resolver that never answers cannot matter.
    BOOST_TEST(fwd.resolveAttempts() == 0u);

    // The first datagram is forwarded, not counted and dropped.
    fwd.forward(makeMsg("the very first"));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "the very first");
}

BOOST_AUTO_TEST_CASE(an_unresponsive_resolver_does_not_hold_up_construction)
{
    // The same property stated the way #38 states it: constructing a Forwarder
    // is not allowed to cost a name lookup, whatever the resolver does. Running
    // the io_context is the only thing that can advance the lookup, so not
    // running it stands in for a resolver that never answers.
    //
    // .invalid never resolves (RFC 2606), so nothing here depends on the
    // network either way.
    Receiver rx;
    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(rx.port());
    cfg.host             = "collector.invalid";

    const auto before = std::chrono::steady_clock::now();
    Forwarder fwd(ioc, cfg);
    const auto elapsed = std::chrono::steady_clock::now() - before;

    // No lookup has come back, because the only thing that can deliver one is
    // the io_context and it has not been run. Deterministic whatever the
    // resolver does on its own thread, which is the whole point.
    BOOST_TEST(fwd.resolveAttempts() == 0u);
    BOOST_TEST(!fwd.resolved());
    // Generous by three orders of magnitude: the point is that no lookup was
    // waited on, not how fast the machine is.
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 1000);

    fwd.stop();
    ioc.run();
}

BOOST_AUTO_TEST_CASE(hostname_is_resolved_and_used)
{
    // Bind wherever "localhost" resolves to first, which is the same address the
    // Forwarder will pick: this asserts that the name was resolved and used, not
    // which family the host happens to prefer.
    boost::asio::io_context resolverIoc;
    boost::asio::ip::udp::resolver resolver(resolverIoc);
    boost::system::error_code ec;
    const auto results = resolver.resolve("localhost", "0", ec);
    BOOST_REQUIRE_MESSAGE(!ec && !results.empty(), "localhost does not resolve on this host");

    ReceiverOn rx(boost::asio::ip::udp::endpoint(results.begin()->endpoint().protocol(), 0));

    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(rx.port());
    cfg.host             = "localhost";
    Forwarder fwd(ioc, cfg);

    // #38: a name costs a lookup, and the constructor only starts it. Nothing
    // has run the io_context yet, so it cannot have finished — the property that
    // keeps a slow resolver away from the UDP bind, asserted here on a name that
    // does resolve rather than only on one that never will.
    BOOST_TEST(!fwd.resolved());

    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("by name"));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "by name");
}

BOOST_AUTO_TEST_CASE(ipv6_destination_is_sent_to)
{
    // The socket's protocol comes from the destination endpoint — here a parsed
    // literal, on the name path a resolved one. It used to be hardcoded to v4, so
    // a v6 destination parsed in the config and then could not be sent to at all.
    boost::asio::io_context probeIoc;
    boost::asio::ip::udp::socket probe(probeIoc);
    boost::system::error_code ec;
    probe.open(boost::asio::ip::udp::v6(), ec);
    if (ec)
    {
        BOOST_TEST_MESSAGE("no IPv6 on this host; skipping");
        return;
    }
    probe.close();

    ReceiverOn rx(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(rx.port());
    cfg.host             = "::1";
    Forwarder fwd(ioc, cfg);
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("over v6"));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "over v6");
}

BOOST_AUTO_TEST_CASE(unresolvable_host_drops_messages_without_failing)
{
    // .invalid never resolves (RFC 2606). Constructing the Forwarder must not
    // throw, forwarding must be off rather than sending somewhere else, and the
    // process must carry on — the whole point of retrying instead of failing.
    Receiver rx;
    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(rx.port());
    cfg.host             = "collector.invalid";

    std::optional<Forwarder> fwd;
    BOOST_REQUIRE_NO_THROW(fwd.emplace(ioc, cfg));

    // Wait for the lookup to come back before asserting anything. Without this
    // the test reaches its assertions while the lookup is still in flight, and
    // passes for the wrong reason — nothing was sent because nothing had been
    // tried yet, rather than because the failure was handled.
    BOOST_REQUIRE(waitAttempts(ioc, *fwd, 1));
    BOOST_CHECK(!fwd->resolved());

    fwd->forward(makeMsg("nowhere to go"));
    drain(ioc);

    BOOST_CHECK(rx.receive().empty());

    // The retry timer would otherwise keep the io_context alive.
    fwd->stop();
    drain(ioc);
}

BOOST_AUTO_TEST_CASE(a_message_sent_before_the_lookup_finishes_is_not_misrouted)
{
    // The documented cost of not blocking the bind on a name lookup: a datagram
    // arriving in the first moments is dropped rather than forwarded, counted,
    // and mentioned when the lookup succeeds.
    //
    // On a name, deliberately: a literal has no such window any more, so running
    // this against one would assert nothing. Which of the two orderings happens
    // is a genuine race — the forward is queued on the strand microseconds after
    // construction, and the lookup's completion is queued whenever the resolver's
    // own thread gets round to it — so this asserts what has to hold either way
    // rather than pretending the race can be won on purpose. What must never
    // happen is the payload going somewhere other than the configured
    // destination, or the forwarder being left unusable by having been given work
    // before it was ready.
    boost::asio::io_context resolverIoc;
    boost::asio::ip::udp::resolver resolver(resolverIoc);
    boost::system::error_code ec;
    const auto results = resolver.resolve("localhost", "0", ec);
    BOOST_REQUIRE_MESSAGE(!ec && !results.empty(), "localhost does not resolve on this host");

    ReceiverOn rx(boost::asio::ip::udp::endpoint(results.begin()->endpoint().protocol(), 0));

    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(rx.port());
    cfg.host             = "localhost";
    Forwarder fwd(ioc, cfg);

    fwd.forward(makeMsg("too early"));
    BOOST_REQUIRE(waitResolved(ioc, fwd));
    drain(ioc);

    const std::string first = rx.receive();
    BOOST_TEST((first.empty() || first == "too early"), "unexpected payload: " << first);

    // Whichever way that went, the forwarder works from here on.
    fwd.forward(makeMsg("after the lookup"));
    drain(ioc);
    BOOST_CHECK_EQUAL(rx.receive(), "after the lookup");
}

BOOST_AUTO_TEST_CASE(an_unresolved_destination_is_retried)
{
    // The failure is reported once and retried in the background, rather than
    // failing the start — a Windows auto-start service is routinely running
    // before DNS is. This covers the retry timer firing and starting a second
    // lookup, which is what "retried" means; the delay before the first retry
    // is what makes this the slowest test in the file.
    Receiver rx;
    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(rx.port());
    cfg.host             = "collector.invalid";
    Forwarder fwd(ioc, cfg);

    BOOST_REQUIRE(waitAttempts(ioc, fwd, 2));
    BOOST_CHECK(!fwd.resolved());

    fwd.stop();
    drain(ioc);
}

BOOST_AUTO_TEST_CASE(stop_lets_the_io_context_run_out_of_work)
{
    // An outstanding retry timer is work, and io_context::run() does not return
    // while there is work — minilog would hang on shutdown waiting for a name
    // that is not coming.
    //
    // The startup lookup is work too, and stop() does not cancel one already in
    // flight: Asio runs getaddrinfo on a thread of its own, and cancel() only
    // reaches operations still queued. run() below therefore returns once that
    // first lookup has come back, which for .invalid is an immediate NXDOMAIN.
    // That delay is accepted and documented rather than engineered around; see
    // Forwarder::stop().
    Receiver rx;
    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(rx.port());
    cfg.host             = "collector.invalid";
    Forwarder fwd(ioc, cfg);

    fwd.stop();

    // No run_for: run() itself must return, which it only does once the timer
    // has been cancelled.
    ioc.run();
    BOOST_CHECK(ioc.stopped());
}

BOOST_AUTO_TEST_CASE(disabled_forwarder_does_not_resolve)
{
    // enabled = false must not look anything up, so a stale host under it costs
    // nothing — which is also why loadConfig does not validate it.
    boost::asio::io_context ioc;
    ForwardingConfig cfg = makeConfig(9999, /*enabled=*/false);
    cfg.host             = "collector.invalid";
    BOOST_REQUIRE_NO_THROW(Forwarder(ioc, cfg));

    ioc.run();
    BOOST_CHECK(ioc.stopped());
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
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("msg", 4));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "msg");
}

BOOST_AUTO_TEST_CASE(matching_facility_is_forwarded)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {4, 16}));
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("kern msg", 4));
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "kern msg");
}

BOOST_AUTO_TEST_CASE(non_matching_facility_is_dropped)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {4}));
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("other msg", 16));
    drain(ioc);

    BOOST_CHECK(rx.receive().empty());
}

BOOST_AUTO_TEST_CASE(no_facility_on_message_dropped_by_non_wildcard_filter)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {4}));
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("malformed msg")); // facility = nullopt
    drain(ioc);

    BOOST_CHECK(rx.receive().empty());
}

BOOST_AUTO_TEST_CASE(no_facility_on_message_forwarded_by_wildcard_filter)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {})); // wildcard
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("malformed msg")); // facility = nullopt
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "malformed msg");
}

BOOST_AUTO_TEST_CASE(excluded_facility_is_not_forwarded_and_the_rest_is)
{
    Receiver rx;
    boost::asio::io_context ioc;
    auto cfg               = makeConfig(rx.port(), true, {}); // wildcard ...
    cfg.excludedFacilities = {19};                            // ... minus local3 (#44)
    Forwarder fwd(ioc, cfg);
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    fwd.forward(makeMsg("local3 msg", 19));
    fwd.forward(makeMsg("local4 msg", 20));
    fwd.forward(makeMsg("malformed msg")); // no facility: exclusions cannot name it
    drain(ioc);

    BOOST_CHECK_EQUAL(rx.receive(), "local4 msg");
    BOOST_CHECK_EQUAL(rx.receive(), "malformed msg");
    BOOST_CHECK(rx.receive().empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Truncation ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(truncation)

BOOST_AUTO_TEST_CASE(no_truncation_when_maxSize_zero)
{
    Receiver rx;
    boost::asio::io_context ioc;
    Forwarder fwd(ioc, makeConfig(rx.port(), true, {}, /*maxMsgSize=*/0));
    BOOST_REQUIRE(waitResolved(ioc, fwd));

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
    BOOST_REQUIRE(waitResolved(ioc, fwd));

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
    BOOST_REQUIRE(waitResolved(ioc, fwd));

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
    BOOST_REQUIRE(waitResolved(ioc, fwd));

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
    BOOST_REQUIRE(waitResolved(ioc, fwd));

    const std::string longMsg(200, 'X');
    fwd.forward(makeMsg(longMsg));
    drain(ioc);

    const std::string got = rx.receive();
    BOOST_CHECK_EQUAL(got, std::string(5, 'X'));
}

BOOST_AUTO_TEST_SUITE_END()
