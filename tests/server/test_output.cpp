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

#define BOOST_TEST_MODULE test_output
#include "output/log_file.hpp"
#include "output/output_manager.hpp"
#include "output/sink_recovery.hpp"

#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace minilog;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace
{

int currentProcessId()
{
#ifdef _WIN32
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

// Unique temp directory per test case.
struct Fixture
{
    fs::path dir;
    boost::asio::io_context ioc;

    Fixture()
    {
        static int counter = 0;
        // The pid keeps concurrent runs apart, and stops a directory left behind
        // by an aborted run from colliding with the next one.
        dir = fs::temp_directory_path() / ("minilog_test_" + std::to_string(currentProcessId()) +
                                           "_" + std::to_string(++counter));
        fs::create_directories(dir);
    }

    ~Fixture()
    {
        // A destructor must not throw, and cleanup can genuinely fail — a test
        // that denies permissions on its own directory is the normal case here.
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Post a write and drain the ioc so the handler completes before returning.
    // restart() is required because poll() marks the ioc stopped when it empties.
    void writeSync(LogFile& lf, const SyslogMessage& msg)
    {
        ioc.restart();
        lf.write(msg);
        ioc.poll();
    }

    std::string readAll(const fs::path& p) const
    {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }

    SyslogMessage
    rfc3164Msg(const std::string& raw = "<34>Oct 11 22:14:15 mymachine su[123]: hello") const
    {
        SyslogMessage msg;
        msg.raw          = raw;
        msg.srcIp        = "192.168.1.50";
        msg.protocol     = Protocol::RFC3164;
        msg.facilityName = "daemon";
        msg.severityName = "NOTICE";
        msg.hostname     = "mymachine";
        msg.appName      = "su";
        msg.procId       = "123";
        msg.timestamp    = "Oct 11 22:14:15";
        msg.message      = "hello";
        return msg;
    }

    // Run the io_context until it runs out of work, or until timeout. Returns
    // whether it ran out. A sink that reopens leaves nothing behind — the retry
    // timer is only re-armed while the sink is still closed — so this returns as
    // soon as a recovery happens and only waits out the whole timeout when one
    // never does.
    bool runUntilIdle(std::chrono::milliseconds timeout)
    {
        ioc.restart();
        const auto start = std::chrono::steady_clock::now();
        ioc.run_for(timeout);
        return std::chrono::steady_clock::now() - start < timeout;
    }

    SyslogMessage unknownMsg(const std::string& raw = "not a syslog message") const
    {
        SyslogMessage msg;
        msg.raw      = raw;
        msg.srcIp    = "10.0.0.1";
        msg.protocol = Protocol::Unknown;
        msg.message  = raw;
        return msg;
    }
};

// rfc3164Msg only varies `raw`, which the JSONL record does not carry. Tests
// that match on the record need the `message` field to vary too.
SyslogMessage messageWith(const std::string& text)
{
    SyslogMessage msg;
    msg.raw          = "<34>Oct 11 22:14:15 mymachine su[123]: " + text;
    msg.srcIp        = "192.168.1.50";
    msg.protocol     = Protocol::RFC3164;
    msg.facilityName = "daemon";
    msg.severityName = "NOTICE";
    msg.hostname     = "mymachine";
    msg.appName      = "su";
    msg.procId       = "123";
    msg.timestamp    = "Oct 11 22:14:15";
    msg.message      = text;
    return msg;
}

OutputConfig sinkConfig(const fs::path& jsonlPath, uint64_t maxSize)
{
    OutputConfig cfg;
    cfg.jsonlFile        = jsonlPath.string();
    cfg.maxSize          = maxSize;
    cfg.maxFiles         = 3;
    cfg.includeMalformed = true;
    return cfg;
}

} // namespace

// ─── Text file ──────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(text_file, Fixture)

BOOST_AUTO_TEST_CASE(single_write_exact_bytes)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("hello world"));

    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), "hello world\n");
}

BOOST_AUTO_TEST_CASE(multiple_writes_appended)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("line one"));
    writeSync(lf, rfc3164Msg("line two"));

    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), "line one\nline two\n");
}

// One datagram is one line, whatever the sender puts in it. An embedded newline
// used to end the record and start a second one that the sender wrote in full —
// PRI included — which nothing reading the file afterwards could tell from a
// genuine entry.
BOOST_AUTO_TEST_CASE(embedded_newline_cannot_forge_a_second_entry)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.includeMalformed = true;

    const std::string forged = "<0>Mar 15 12:00:00 host sshd[1]: root login SUCCEEDED";

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("<14>Mar 15 12:00:00 host real: benign\n" + forged));

    const auto contents = readAll(dir / "syslog.log");
    BOOST_CHECK_EQUAL(contents, "<14>Mar 15 12:00:00 host real: benign\\n" + forged + "\n");
    BOOST_CHECK_EQUAL(std::count(contents.begin(), contents.end(), '\n'), 1);
}

BOOST_AUTO_TEST_CASE(escaping_does_not_break_rotation_accounting)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.maxSize          = 8;
    cfg.maxFiles         = 3;
    cfg.includeMalformed = true;

    // "a\nb" is three bytes in, five out. Rotation counts what was written, so
    // the second write must land in a fresh file rather than being sized off the
    // unescaped length.
    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("a\nb\nc"));
    writeSync(lf, rfc3164Msg("second"));

    BOOST_CHECK_EQUAL(readAll(dir / "syslog.1.log"), "a\\nb\\nc\n");
    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), "second\n");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── JSONL file ─────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(jsonl_file, Fixture)

BOOST_AUTO_TEST_CASE(single_write_valid_json)
{
    OutputConfig cfg;
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg());

    const std::string content = readAll(dir / "syslog.jsonl");
    BOOST_REQUIRE(!content.empty());
    BOOST_CHECK_EQUAL(content.back(), '\n');

    // Strip trailing newline before parsing.
    auto obj = bj::parse(std::string(content.begin(), content.end() - 1)).as_object();

    BOOST_CHECK_EQUAL(obj["src"].as_string(), "192.168.1.50");
    BOOST_CHECK_EQUAL(obj["proto"].as_string(), "RFC3164");
    BOOST_CHECK_EQUAL(obj["facility"].as_string(), "daemon");
    BOOST_CHECK_EQUAL(obj["severity"].as_string(), "NOTICE");
    BOOST_CHECK_EQUAL(obj["hostname"].as_string(), "mymachine");
    BOOST_CHECK_EQUAL(obj["app"].as_string(), "su");
    BOOST_CHECK_EQUAL(obj["pid"].as_string(), "123");
    BOOST_CHECK(obj["msgid"].is_null());
    BOOST_CHECK_EQUAL(obj["msg_time"].as_string(), "Oct 11 22:14:15");
    BOOST_CHECK_EQUAL(obj["message"].as_string(), "hello");
    BOOST_CHECK(!obj["rcv"].as_string().empty());
}

BOOST_AUTO_TEST_CASE(rfc5424_msgid_present)
{
    OutputConfig cfg;
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.includeMalformed = true;

    SyslogMessage msg;
    msg.raw          = "<34>1 2026-01-01T00:00:00Z host app 123 ID47 - hello";
    msg.srcIp        = "10.0.0.1";
    msg.protocol     = Protocol::RFC5424;
    msg.facilityName = "daemon";
    msg.severityName = "NOTICE";
    msg.hostname     = "host";
    msg.appName      = "app";
    msg.procId       = "123";
    msg.msgId        = "ID47";
    msg.message      = "hello";

    LogFile lf(ioc, cfg);
    writeSync(lf, msg);

    const std::string content = readAll(dir / "syslog.jsonl");
    auto obj = bj::parse(std::string(content.begin(), content.end() - 1)).as_object();

    BOOST_CHECK_EQUAL(obj["proto"].as_string(), "RFC5424");
    BOOST_CHECK_EQUAL(obj["msgid"].as_string(), "ID47");
}

BOOST_AUTO_TEST_CASE(rfc5424_msg_time_present)
{
    // RFC 5424 timestamp should appear verbatim in msg_time.
    OutputConfig cfg;
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.includeMalformed = true;

    SyslogMessage msg;
    msg.raw          = "<34>1 2026-01-01T12:34:56.789Z host app 99 - - hello";
    msg.srcIp        = "10.0.0.1";
    msg.protocol     = Protocol::RFC5424;
    msg.facilityName = "daemon";
    msg.severityName = "NOTICE";
    msg.hostname     = "host";
    msg.appName      = "app";
    msg.procId       = "99";
    msg.timestamp    = "2026-01-01T12:34:56.789Z";
    msg.message      = "hello";

    LogFile lf(ioc, cfg);
    writeSync(lf, msg);

    const std::string content = readAll(dir / "syslog.jsonl");
    auto obj = bj::parse(std::string(content.begin(), content.end() - 1)).as_object();

    BOOST_CHECK_EQUAL(obj["msg_time"].as_string(), "2026-01-01T12:34:56.789Z");
}

BOOST_AUTO_TEST_CASE(msg_time_null_when_absent)
{
    // A message with no parsed timestamp should yield msg_time: null.
    OutputConfig cfg;
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.includeMalformed = true;

    SyslogMessage msg = rfc3164Msg();
    msg.timestamp.reset(); // explicitly absent

    LogFile lf(ioc, cfg);
    writeSync(lf, msg);

    const std::string content = readAll(dir / "syslog.jsonl");
    auto obj = bj::parse(std::string(content.begin(), content.end() - 1)).as_object();

    BOOST_CHECK(obj["msg_time"].is_null());
}

BOOST_AUTO_TEST_CASE(unknown_protocol_nulls)
{
    OutputConfig cfg;
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, unknownMsg("garbage bytes"));

    const std::string content = readAll(dir / "syslog.jsonl");
    auto obj = bj::parse(std::string(content.begin(), content.end() - 1)).as_object();

    BOOST_CHECK_EQUAL(obj["proto"].as_string(), "UNKNOWN");
    BOOST_CHECK(obj["facility"].is_null());
    BOOST_CHECK(obj["severity"].is_null());
    BOOST_CHECK(obj["hostname"].is_null());
    BOOST_CHECK(obj["app"].is_null());
    BOOST_CHECK(obj["pid"].is_null());
    BOOST_CHECK(obj["msgid"].is_null());
    BOOST_CHECK(obj["msg_time"].is_null());
    BOOST_CHECK_EQUAL(obj["message"].as_string(), "garbage bytes");
}

BOOST_AUTO_TEST_CASE(invalid_utf8_replaced_with_replacement_character)
{
    // Syslog datagrams can contain non-UTF-8 bytes (e.g. Latin-1 sources).
    // sanitizeUtf8() replaces each invalid byte sequence with U+FFFD before
    // the fields are handed to boost::json::serialize.
    OutputConfig cfg;
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.includeMalformed = true;

    SyslogMessage msg = rfc3164Msg();
    msg.hostname      = "host\xFF"
                        "name";    // 0xFF is never valid UTF-8
    msg.message       = "caf\xe9"; // Latin-1 é (incomplete UTF-8 sequence)

    LogFile lf(ioc, cfg);
    writeSync(lf, msg);

    const std::string content = readAll(dir / "syslog.jsonl");
    BOOST_REQUIRE(!content.empty());

    // Output must be parseable as JSON.
    auto obj = bj::parse(std::string(content.begin(), content.end() - 1)).as_object();

    // Each invalid byte is replaced with U+FFFD (UTF-8: 0xEF 0xBF 0xBD).
    constexpr std::string_view kReplacement = "\xef\xbf\xbd";
    const std::string hostname              = std::string(obj["hostname"].as_string());
    BOOST_CHECK(hostname.find(kReplacement) != std::string::npos);
    BOOST_CHECK_EQUAL(hostname.find('\xff'), std::string::npos);

    const std::string message = std::string(obj["message"].as_string());
    BOOST_CHECK(message.find(kReplacement) != std::string::npos);
    BOOST_CHECK_EQUAL(message.find('\xe9'), std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── include_malformed ──────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(malformed_filter, Fixture)

BOOST_AUTO_TEST_CASE(include_malformed_false_skips_unknown)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.includeMalformed = false;

    LogFile lf(ioc, cfg);
    writeSync(lf, unknownMsg("bad data"));

    // File should not exist (never written).
    BOOST_CHECK(!fs::exists(dir / "syslog.log"));
}

BOOST_AUTO_TEST_CASE(include_malformed_false_passes_known)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.includeMalformed = false;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("good message"));

    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), "good message\n");
}

BOOST_AUTO_TEST_CASE(include_malformed_true_keeps_unknown)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, unknownMsg("bad data"));

    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), "bad data\n");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Rotation ───────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(rotation, Fixture)

BOOST_AUTO_TEST_CASE(text_file_rotates_on_size)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.maxSize          = 1; // trigger after every write
    cfg.maxFiles         = 0; // unlimited
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("hello")); // write 1 → current
    writeSync(lf, rfc3164Msg("hello")); // write 2 → rotates to .1, writes to new current

    BOOST_CHECK(fs::exists(dir / "syslog.log"));
    BOOST_CHECK(fs::exists(dir / "syslog.1.log"));
    BOOST_CHECK(!fs::exists(dir / "syslog.2.log"));
}

BOOST_AUTO_TEST_CASE(both_files_rotate_together)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.maxSize          = 1;
    cfg.maxFiles         = 0;
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("hello"));
    writeSync(lf, rfc3164Msg("hello")); // triggers rotation

    BOOST_CHECK(fs::exists(dir / "syslog.1.log"));
    BOOST_CHECK(fs::exists(dir / "syslog.1.jsonl"));
}

BOOST_AUTO_TEST_CASE(rotation_numbering_shifts)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.maxSize          = 1;
    cfg.maxFiles         = 0;
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("A"));
    writeSync(lf, rfc3164Msg("B")); // A → .1
    writeSync(lf, rfc3164Msg("C")); // B → .2, A → .2... wait

    // After 3 writes:
    //   .2 = first write's content ("A\n")
    //   .1 = second write's content ("B\n")
    //   current = third write's content ("C\n")
    BOOST_CHECK_EQUAL(readAll(dir / "syslog.2.log"), "A\n");
    BOOST_CHECK_EQUAL(readAll(dir / "syslog.1.log"), "B\n");
    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log"), "C\n");
    BOOST_CHECK(!fs::exists(dir / "syslog.3.log"));
}

BOOST_AUTO_TEST_CASE(max_files_deletes_oldest)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.maxSize          = 1;
    cfg.maxFiles         = 3;
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    for (int i = 0; i < 5; ++i)
    {
        writeSync(lf, rfc3164Msg("x"));
    }

    BOOST_CHECK(fs::exists(dir / "syslog.1.log"));
    BOOST_CHECK(fs::exists(dir / "syslog.2.log"));
    BOOST_CHECK(fs::exists(dir / "syslog.3.log"));
    BOOST_CHECK(!fs::exists(dir / "syslog.4.log"));
    BOOST_CHECK(!fs::exists(dir / "syslog.5.log"));
}

BOOST_AUTO_TEST_CASE(max_files_zero_keeps_all)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.maxSize          = 1;
    cfg.maxFiles         = 0; // unlimited
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    for (int i = 0; i < 6; ++i)
    {
        writeSync(lf, rfc3164Msg("x"));
    }

    // After 6 writes: current + .1 through .5
    BOOST_CHECK(fs::exists(dir / "syslog.5.log"));
    BOOST_CHECK(!fs::exists(dir / "syslog.6.log")); // 6th write goes to current
}

BOOST_AUTO_TEST_CASE(jsonl_only_rotates_on_size)
{
    // When only jsonl_file is configured, rotation triggers on jsonl size.
    OutputConfig cfg;
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.maxSize          = 1; // any JSONL record exceeds 1 byte
    cfg.maxFiles         = 0;
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    writeSync(lf, rfc3164Msg("hello")); // first write → current
    writeSync(lf, rfc3164Msg("hello")); // jsonl size >= maxSize → rotates to .1

    BOOST_CHECK(fs::exists(dir / "syslog.jsonl"));
    BOOST_CHECK(fs::exists(dir / "syslog.1.jsonl"));
    BOOST_CHECK(!fs::exists(dir / "syslog.2.jsonl"));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Edge cases ─────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(edge_cases, Fixture)

BOOST_AUTO_TEST_CASE(no_crash_on_missing_directory)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "nonexistent" / "syslog.log").string();
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    // Must not throw or crash.
    BOOST_CHECK_NO_THROW(writeSync(lf, rfc3164Msg("hello")));
    BOOST_CHECK(!fs::exists(dir / "nonexistent" / "syslog.log"));
}

BOOST_AUTO_TEST_CASE(max_size_zero_never_rotates)
{
    OutputConfig cfg;
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.maxSize          = 0; // unlimited
    cfg.includeMalformed = true;

    LogFile lf(ioc, cfg);
    for (int i = 0; i < 10; ++i)
    {
        writeSync(lf, rfc3164Msg("x"));
    }

    BOOST_CHECK(!fs::exists(dir / "syslog.1.log"));
    BOOST_CHECK_EQUAL(readAll(dir / "syslog.log").size(), 10u * 2u); // "x\n" × 10
}

BOOST_AUTO_TEST_SUITE_END()

// ─── OutputManager routing ───────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(output_manager_routing, Fixture)

namespace
{

// Build a three-sink Config: "main" (wildcard), "auth" (auth+authpriv), "mail" (mail only).
// Facility numbers per RFC5424: mail=2, auth=4, authpriv=10.
Config makeRoutingConfig(const fs::path& base)
{
    Config cfg;

    OutputConfig mainOut;
    mainOut.name             = "main";
    mainOut.textFile         = (base / "main.log").string();
    mainOut.includeMalformed = true;
    // facilities empty = wildcard

    OutputConfig authOut;
    authOut.name             = "auth";
    authOut.textFile         = (base / "auth.log").string();
    authOut.includeMalformed = true;
    authOut.facilities       = {4, 10}; // auth, authpriv

    OutputConfig mailOut;
    mailOut.name             = "mail";
    mailOut.textFile         = (base / "mail.log").string();
    mailOut.includeMalformed = true;
    mailOut.facilities       = {2}; // mail

    cfg.outputs = {mainOut, authOut, mailOut};
    return cfg;
}

} // namespace

BOOST_AUTO_TEST_CASE(wildcard_receives_all_facilities)
{
    auto cfg = makeRoutingConfig(dir);
    OutputManager om(ioc, cfg);

    auto msg     = rfc3164Msg("hello");
    msg.facility = 4; // auth
    ioc.restart();
    om.dispatch(msg);
    ioc.poll();

    BOOST_CHECK(!readAll(dir / "main.log").empty()); // wildcard
    BOOST_CHECK(!readAll(dir / "auth.log").empty()); // auth matches
    BOOST_CHECK(readAll(dir / "mail.log").empty());  // mail does not
}

BOOST_AUTO_TEST_CASE(facility_specific_sink_not_reached_by_other_facility)
{
    auto cfg = makeRoutingConfig(dir);
    OutputManager om(ioc, cfg);

    auto msg     = rfc3164Msg("hello");
    msg.facility = 2; // mail
    ioc.restart();
    om.dispatch(msg);
    ioc.poll();

    BOOST_CHECK(!readAll(dir / "main.log").empty()); // wildcard
    BOOST_CHECK(readAll(dir / "auth.log").empty());  // auth does not match
    BOOST_CHECK(!readAll(dir / "mail.log").empty()); // mail matches
}

BOOST_AUTO_TEST_CASE(unknown_protocol_reaches_only_wildcard)
{
    auto cfg = makeRoutingConfig(dir);
    OutputManager om(ioc, cfg);

    ioc.restart();
    om.dispatch(unknownMsg("garbage")); // no facility on UNKNOWN messages
    ioc.poll();

    BOOST_CHECK(!readAll(dir / "main.log").empty()); // wildcard
    BOOST_CHECK(readAll(dir / "auth.log").empty());  // no facility → no match
    BOOST_CHECK(readAll(dir / "mail.log").empty());  // no facility → no match
}

BOOST_AUTO_TEST_CASE(include_malformed_false_blocks_unknown)
{
    Config cfg;
    OutputConfig out;
    out.name             = "main";
    out.textFile         = (dir / "main.log").string();
    out.includeMalformed = false;
    cfg.outputs          = {out};

    OutputManager om(ioc, cfg);
    ioc.restart();
    om.dispatch(unknownMsg("garbage"));
    ioc.poll();

    BOOST_CHECK(!fs::exists(dir / "main.log"));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── sanitizeUtf8 ────────────────────────────────────────────────────────────
//
// Tests call sanitizeUtf8() directly (declared in log_file.hpp).
// The function must pass valid UTF-8 through unchanged and replace every
// invalid byte sequence with U+FFFD (0xEF 0xBF 0xBD).

BOOST_AUTO_TEST_SUITE(sanitize_utf8)

namespace
{
const std::string R = "\xef\xbf\xbd"; // U+FFFD replacement character
} // namespace

// ── Valid sequences — must pass through unchanged ────────────────────────────

BOOST_AUTO_TEST_CASE(empty_string)
{
    BOOST_CHECK_EQUAL(sanitizeUtf8(""), "");
}

BOOST_AUTO_TEST_CASE(pure_ascii_unchanged)
{
    BOOST_CHECK_EQUAL(sanitizeUtf8("Hello, world!"), "Hello, world!");
}

BOOST_AUTO_TEST_CASE(valid_2byte_unchanged)
{
    // U+00E9 LATIN SMALL LETTER E WITH ACUTE: 0xC3 0xA9
    const std::string in = "caf\xc3\xa9";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_3byte_unchanged)
{
    // U+20AC EURO SIGN: 0xE2 0x82 0xAC
    const std::string in = "\xe2\x82\xac";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_4byte_unchanged)
{
    // U+1D11E MUSICAL SYMBOL G CLEF: 0xF0 0x9D 0x84 0x9E
    const std::string in = "\xf0\x9d\x84\x9e";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_3byte_ee_range_unchanged)
{
    // U+E000 (Private Use Area): 0xEE 0x80 0x80 — tests the 0xEE–0xEF lead branch.
    const std::string in = "\xee\x80\x80";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_4byte_f1_range_unchanged)
{
    // U+40000: 0xF1 0x80 0x80 0x80 — tests the 0xF1–0xF3 lead branch.
    const std::string in = "\xf1\x80\x80\x80";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_3byte_e0_boundary_unchanged)
{
    // U+0800 — minimum code point that uses a 3-byte sequence with 0xE0 lead.
    // First continuation must be >= 0xA0; anything lower is an overlong encoding.
    const std::string in = "\xe0\xa0\x80";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_3byte_ed_boundary_unchanged)
{
    // U+D7FF — last code point before the surrogate range.
    // 0xED lead requires first continuation <= 0x9F.
    const std::string in = "\xed\x9f\xbf";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_4byte_f0_boundary_unchanged)
{
    // U+10000 — minimum 4-byte sequence.
    // 0xF0 lead requires first continuation >= 0x90.
    const std::string in = "\xf0\x90\x80\x80";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

BOOST_AUTO_TEST_CASE(valid_4byte_f4_boundary_unchanged)
{
    // U+10FFFF — last valid Unicode code point.
    // 0xF4 lead requires first continuation <= 0x8F.
    const std::string in = "\xf4\x8f\xbf\xbf";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), in);
}

// ── Invalid lead bytes ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(invalid_lead_0xff_replaced)
{
    // 0xFF is never a valid lead byte.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xff"), R);
}

BOOST_AUTO_TEST_CASE(stray_continuation_byte_replaced)
{
    // 0x80–0xBF can only appear as continuation bytes; a lone one is invalid.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\x80"), R);
}

BOOST_AUTO_TEST_CASE(overlong_lead_bytes_replaced)
{
    // 0xC0 and 0xC1 would only produce overlong encodings of ASCII — rejected.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xc0\x80"), R + R);
}

// ── Truncated sequences ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(truncated_2byte_at_end_replaced)
{
    // A 2-byte lead byte (0xC3) with no continuation following.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xc3"), R);
}

BOOST_AUTO_TEST_CASE(truncated_3byte_at_end_replaced)
{
    // 0xE2 alone — truncated before either continuation byte arrives.
    // The stray 0x82 left behind is also an invalid lead byte.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xe2\x82"), R + R);
}

BOOST_AUTO_TEST_CASE(truncated_4byte_at_end_replaced)
{
    // 0xF0 with only two of its three required continuation bytes.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xf0\x90\x80"), R + R + R);
}

// ── Out-of-range first continuation byte ────────────────────────────────────

BOOST_AUTO_TEST_CASE(overlong_3byte_e0_replaced)
{
    // 0xE0 0x9F … — first continuation below 0xA0 is an overlong encoding.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xe0\x9f\x80"), R + R + R);
}

BOOST_AUTO_TEST_CASE(surrogate_ed_replaced)
{
    // 0xED 0xA0 0x80 encodes U+D800 — a surrogate pair value, not valid UTF-8.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xed\xa0\x80"), R + R + R);
}

BOOST_AUTO_TEST_CASE(overlong_4byte_f0_replaced)
{
    // 0xF0 0x8F … — first continuation below 0x90 is an overlong encoding.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xf0\x8f\x80\x80"), R + R + R + R);
}

BOOST_AUTO_TEST_CASE(out_of_range_4byte_f4_replaced)
{
    // 0xF4 0x90 … — first continuation above 0x8F exceeds Unicode's U+10FFFF limit.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xf4\x90\x80\x80"), R + R + R + R);
}

// ── Bad later continuation bytes ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(bad_second_continuation_in_3byte_replaced)
{
    // 0xE2 0x82 0x41: valid lead + valid first cont, then 'A' instead of 0x80–0xBF.
    // The lead is replaced; the stray 0x82 is then also replaced; 'A' passes through.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xe2\x82\x41"), R + R + "A");
}

BOOST_AUTO_TEST_CASE(bad_third_continuation_in_4byte_replaced)
{
    // 0xF0 0x90 0x80 0x41: three valid bytes then 'A' instead of 0x80–0xBF.
    BOOST_CHECK_EQUAL(sanitizeUtf8("\xf0\x90\x80\x41"), R + R + R + "A");
}

// ── Mixed content ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(invalid_byte_embedded_in_ascii)
{
    // A single invalid byte in the middle of an otherwise clean ASCII string.
    BOOST_CHECK_EQUAL(sanitizeUtf8("hello\xffworld"), "hello" + R + "world");
}

BOOST_AUTO_TEST_CASE(valid_multibyte_and_invalid_interleaved)
{
    // Valid 2-byte (é), then an invalid byte, then valid 3-byte (€).
    const std::string in = "\xc3\xa9\xff\xe2\x82\xac";
    BOOST_CHECK_EQUAL(sanitizeUtf8(in), "\xc3\xa9" + R + "\xe2\x82\xac");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── escapeControlChars ──────────────────────────────────────────────────────
//
// Tests call escapeControlChars() directly (declared in log_file.hpp). It must
// leave one datagram unable to produce more than one line in the text sink,
// stay reversible, and leave anything above 0x7F alone.

BOOST_AUTO_TEST_SUITE(escape_control_chars)

// ── Ordinary text — must pass through unchanged ──────────────────────────────

BOOST_AUTO_TEST_CASE(empty_string)
{
    BOOST_CHECK_EQUAL(escapeControlChars(""), "");
}

BOOST_AUTO_TEST_CASE(printable_ascii_unchanged)
{
    const std::string in = "<34>Oct 11 22:14:15 mymachine su[123]: hello, world!";
    BOOST_CHECK_EQUAL(escapeControlChars(in), in);
}

BOOST_AUTO_TEST_CASE(tab_stays_literal)
{
    BOOST_CHECK_EQUAL(escapeControlChars("a\tb"), "a\tb");
}

// ── Line-forging characters ──────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(line_feed_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars("a\nb"), "a\\nb");
}

BOOST_AUTO_TEST_CASE(carriage_return_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars("a\rb"), "a\\rb");
}

BOOST_AUTO_TEST_CASE(crlf_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars("a\r\nb"), "a\\r\\nb");
}

BOOST_AUTO_TEST_CASE(nul_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars(std::string_view("a\0b", 3)), "a\\x00b");
}

BOOST_AUTO_TEST_CASE(escape_character_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars("a\x1b[2Jb"), "a\\x1B[2Jb");
}

BOOST_AUTO_TEST_CASE(del_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars(std::string("a\x7f") + "b"), "a\\x7Fb");
}

BOOST_AUTO_TEST_CASE(every_c0_except_tab_escaped)
{
    for (int c = 0x00; c <= 0x1F; ++c)
    {
        const std::string in(1, static_cast<char>(c));
        const std::string out = escapeControlChars(in);
        if (c == '\t')
        {
            BOOST_CHECK_EQUAL(out, in);
        }
        else
        {
            BOOST_CHECK_MESSAGE(out.size() > 1 && out[0] == '\\',
                                "0x" << std::hex << c << " reached the file unescaped");
        }
    }
}

// ── Reversibility ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(backslash_doubled)
{
    BOOST_CHECK_EQUAL(escapeControlChars("a\\b"), "a\\\\b");
}

// A literal backslash-n in the datagram must not come out looking like an
// escaped newline; without doubling the backslash the two would be the same
// bytes and the transform would not be reversible.
BOOST_AUTO_TEST_CASE(literal_backslash_n_distinct_from_escaped_newline)
{
    BOOST_CHECK_NE(escapeControlChars("a\\nb"), escapeControlChars("a\nb"));
    BOOST_CHECK_EQUAL(escapeControlChars("a\\nb"), "a\\\\nb");
}

BOOST_AUTO_TEST_CASE(hex_escape_is_always_two_digits)
{
    // "\x1B" followed by hex digits: a C compiler would swallow "BAD" into the
    // escape, a reader of this format takes exactly two digits and stops.
    BOOST_CHECK_EQUAL(escapeControlChars(std::string("\x1b") + "BAD"), "\\x1BBAD");
}

// ── C1 controls — the 8-bit forms of the same sequences ──────────────────────
//
// U+009B is CSI and U+009D is OSC, so a sender reaches the escape sequences the
// C0 branch exists to stop without ever sending an ESC byte. They arrive as the
// two-byte UTF-8 sequence 0xC2 0x80-0xC2 0x9F.

namespace
{
// U+009B CSI, as it arrives on the wire.
const std::string CSI = "\xc2\x9b";
const std::string OSC = "\xc2\x9d"; // U+009D
} // namespace

BOOST_AUTO_TEST_CASE(c1_csi_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars(CSI + "2J"), "\\u009B2J");
}

BOOST_AUTO_TEST_CASE(c1_osc_escaped)
{
    BOOST_CHECK_EQUAL(escapeControlChars(OSC + "0;title"), "\\u009D0;title");
}

BOOST_AUTO_TEST_CASE(every_c1_escaped)
{
    for (int cp = 0x80; cp <= 0x9F; ++cp)
    {
        const std::string in = "\xc2" + std::string(1, static_cast<char>(cp));
        BOOST_CHECK_EQUAL(escapeControlChars(in), std::format("\\u{:04X}", cp));
    }
}

// U+00A0 is the first codepoint past C1 and must be left alone, or the escaping
// starts eating ordinary Latin-1 text.
BOOST_AUTO_TEST_CASE(codepoint_just_past_c1_unchanged)
{
    const std::string in = "\xc2\xa0"; // U+00A0 NO-BREAK SPACE
    BOOST_CHECK_EQUAL(escapeControlChars(in), in);
}

// The whole point of matching the decoded codepoint rather than the raw byte:
// 0x97 here is a continuation byte of U+65E5, not a C1 control.
BOOST_AUTO_TEST_CASE(continuation_bytes_in_the_c1_range_unchanged)
{
    const std::string in = "\xe6\x97\xa5"; // U+65E5 日 — the 0x97 must survive
    BOOST_CHECK_EQUAL(escapeControlChars(in), in);
}

// A truncated sequence must not read past the end of the input.
BOOST_AUTO_TEST_CASE(trailing_lead_byte_unchanged)
{
    const std::string in = "text\xc2";
    BOOST_CHECK_EQUAL(escapeControlChars(in), in);
}

// A bare 0x9B is not valid UTF-8 and is left as the raw byte it is: escaping
// every byte in that range is what would mangle the continuation bytes above.
BOOST_AUTO_TEST_CASE(bare_c1_byte_unchanged)
{
    const std::string in = "\x9b";
    BOOST_CHECK_EQUAL(escapeControlChars(in), in);
}

// ── Non-ASCII — must pass through byte for byte ──────────────────────────────

BOOST_AUTO_TEST_CASE(utf8_unchanged)
{
    const std::string in = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"; // 日本語
    BOOST_CHECK_EQUAL(escapeControlChars(in), in);
}

BOOST_AUTO_TEST_CASE(eight_bit_bytes_unchanged)
{
    const std::string in = "\x80\xff\xc3\xa9";
    BOOST_CHECK_EQUAL(escapeControlChars(in), in);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Sink recovery policy ────────────────────────────────────────────────────
//
// When a closed sink retries, and how often the outage is reported while it
// lasts. Unit level with an injected clock, because a real filesystem fault
// cannot be made to last a controlled number of seconds.

BOOST_AUTO_TEST_SUITE(sink_recovery_policy)

BOOST_AUTO_TEST_CASE(the_failure_that_closes_a_sink_is_reported_at_once)
{
    SinkRecovery recovery;
    const auto now = std::chrono::steady_clock::now();

    const auto decision = recovery.onFailure("write failed", now);

    BOOST_TEST(decision.report);
    BOOST_TEST(decision.firstFailure);
    BOOST_TEST(decision.suppressed == 0u);
    BOOST_TEST(recovery.closed());
}

BOOST_AUTO_TEST_CASE(failed_reopens_within_the_interval_are_silent)
{
    SinkRecovery recovery;
    const auto now = std::chrono::steady_clock::now();
    recovery.onFailure("open failed", now);

    for (int i = 0; i < 20; ++i)
    {
        // Same instant every time: nothing here may depend on the test being
        // slow enough for the report interval to elapse.
        const auto decision = recovery.onFailure("open failed", now);
        BOOST_TEST(!decision.report, "attempt " << i << " was reported");
    }
}

BOOST_AUTO_TEST_CASE(a_summary_is_reported_once_the_interval_has_passed)
{
    SinkRecovery recovery;
    const auto start = std::chrono::steady_clock::now();
    recovery.onFailure("open failed", start);
    recovery.onFailure("open failed", start);
    recovery.onFailure("open failed", start);

    const auto decision = recovery.onFailure("open failed", start + SinkRecovery::kReportInterval);

    BOOST_TEST(decision.report);
    BOOST_TEST(!decision.firstFailure);
    // The failure that closed the sink was reported on its own, so it is not in
    // the count: the two silent attempts after it, plus this one.
    BOOST_TEST(decision.suppressed == 3u);
    BOOST_TEST(!decision.varied);
    BOOST_TEST(decision.closedFor.count() == SinkRecovery::kReportInterval.count());
}

BOOST_AUTO_TEST_CASE(a_summary_covering_two_reasons_says_so)
{
    // The line names one reason, so a bare count would claim the attempts it
    // covers all failed that way.
    SinkRecovery recovery;
    const auto start = std::chrono::steady_clock::now();
    recovery.onFailure("open failed", start);
    recovery.onFailure("open failed", start);
    recovery.onFailure("cannot determine size", start);

    const auto decision = recovery.onFailure("open failed", start + SinkRecovery::kReportInterval);

    BOOST_TEST(decision.report);
    BOOST_TEST(decision.varied);
}

BOOST_AUTO_TEST_CASE(the_report_clock_restarts_after_each_summary)
{
    SinkRecovery recovery;
    const auto start = std::chrono::steady_clock::now();
    recovery.onFailure("open failed", start);

    const auto first = recovery.onFailure("open failed", start + SinkRecovery::kReportInterval);
    BOOST_REQUIRE(first.report);

    // One tick later is not another interval.
    const auto tooSoon = recovery.onFailure(
        "open failed", start + SinkRecovery::kReportInterval + std::chrono::seconds{1});
    BOOST_TEST(!tooSoon.report);

    const auto second =
        recovery.onFailure("open failed", start + (2 * SinkRecovery::kReportInterval));
    BOOST_TEST(second.report);
    // Only what happened since the previous summary, which is the point of the
    // count: the two above.
    BOOST_TEST(second.suppressed == 2u);
    BOOST_TEST(second.closedFor.count() == 2 * SinkRecovery::kReportInterval.count());
}

BOOST_AUTO_TEST_CASE(recovery_reports_the_length_of_the_outage)
{
    SinkRecovery recovery;
    const auto start = std::chrono::steady_clock::now();
    recovery.onFailure("open failed", start);
    recovery.onFailure("open failed", start + std::chrono::seconds{30});

    const auto outage = recovery.onRecovered(start + std::chrono::seconds{60});

    BOOST_TEST(outage.closedFor.count() == 60);
    // The failure that closed the sink plus the one failed reopen.
    BOOST_TEST(outage.failures == 2u);
    BOOST_TEST(!recovery.closed());
}

BOOST_AUTO_TEST_CASE(a_sink_that_fails_again_after_recovering_starts_a_new_outage)
{
    // Otherwise the second outage would be reported as a continuation of the
    // first — silent until the old report interval elapsed, and with a duration
    // measured from a fault that has already been fixed.
    SinkRecovery recovery;
    const auto start = std::chrono::steady_clock::now();
    recovery.onFailure("open failed", start);
    recovery.onRecovered(start + std::chrono::seconds{10});

    const auto decision = recovery.onFailure("write failed", start + std::chrono::seconds{11});

    BOOST_TEST(decision.report);
    BOOST_TEST(decision.firstFailure);
    BOOST_TEST(decision.closedFor.count() == 0);
}

BOOST_AUTO_TEST_CASE(recovering_a_sink_that_was_never_closed_reports_nothing)
{
    SinkRecovery recovery;
    const auto outage = recovery.onRecovered(std::chrono::steady_clock::now());
    BOOST_TEST(outage.failures == 0u);
    BOOST_TEST(outage.closedFor.count() == 0);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Sink recovery, end to end ───────────────────────────────────────────────
//
// A sink closed by a filesystem error used to stay closed until the process was
// restarted, so a storage fault lasting seconds cost one facility's log for as
// long as it took somebody to notice. These drive the real timer with the
// interval shortened, and provoke the fault with a missing directory so they run
// on Windows as well as POSIX.

BOOST_FIXTURE_TEST_SUITE(sink_recovery_end_to_end, Fixture)

namespace
{

// Short enough to keep these tests quick, long enough that a loaded machine
// still runs several attempts inside the timeouts below.
constexpr std::chrono::milliseconds kTestRetry{20};
// Generous: it bounds how long a *passing* test waits only when recovery never
// happens, since run_for returns as soon as the sink reopens.
constexpr std::chrono::milliseconds kRecoveryTimeout{5000};

} // namespace

BOOST_AUTO_TEST_CASE(a_closed_sink_reopens_once_its_directory_appears)
{
    const auto subdir = dir / "provisioned_late";
    auto cfg          = sinkConfig(subdir / "syslog.jsonl", 0);
    cfg.name          = "main";

    LogFile lf(ioc, cfg, kTestRetry);

    // The directory does not exist yet, so the lazy open on the first write
    // fails and closes the sink.
    writeSync(lf, messageWith("lost"));
    BOOST_REQUIRE(!fs::exists(cfg.jsonlFile));

    fs::create_directories(subdir);
    BOOST_REQUIRE(runUntilIdle(kRecoveryTimeout));

    // Reopening does not replay the message that was dropped; what it restores
    // is the sink, so the next one lands.
    writeSync(lf, messageWith("kept"));
    const auto contents = readAll(cfg.jsonlFile);
    BOOST_CHECK(contents.find("kept") != std::string::npos);
    BOOST_CHECK(contents.find("lost") == std::string::npos);

    lf.close();
    ioc.restart();
    ioc.poll();
}

BOOST_AUTO_TEST_CASE(a_sink_whose_fault_never_clears_keeps_retrying)
{
    auto cfg = sinkConfig(dir / "still_missing" / "syslog.jsonl", 0);
    cfg.name = "main";

    LogFile lf(ioc, cfg, kTestRetry);
    writeSync(lf, messageWith("dropped"));

    // Never runs out of work, because every failed attempt arms the timer again.
    BOOST_CHECK(!runUntilIdle(std::chrono::milliseconds{300}));
    BOOST_CHECK(!fs::exists(cfg.jsonlFile));

    // And it is still the same sink, so it recovers whenever the fault does.
    fs::create_directories(dir / "still_missing");
    BOOST_REQUIRE(runUntilIdle(kRecoveryTimeout));
    writeSync(lf, messageWith("kept"));
    BOOST_CHECK(readAll(cfg.jsonlFile).find("kept") != std::string::npos);

    lf.close();
    ioc.restart();
    ioc.poll();
}

BOOST_AUTO_TEST_CASE(closing_a_sink_during_an_outage_stops_the_retry)
{
    // A pending retry is outstanding work, so a shutdown that left one armed
    // would wait out the interval — and the attempt after it would reopen files
    // that nothing is going to write to.
    const auto subdir = dir / "closed_during_outage";
    auto cfg          = sinkConfig(subdir / "syslog.jsonl", 0);
    cfg.name          = "main";

    LogFile lf(ioc, cfg, kTestRetry);
    writeSync(lf, messageWith("dropped"));

    lf.close();
    ioc.restart();
    ioc.poll();

    // Fixing the fault now must not bring the sink back: it was closed for
    // shutdown, not by the fault.
    fs::create_directories(subdir);
    BOOST_CHECK(runUntilIdle(std::chrono::milliseconds{300}));
    writeSync(lf, messageWith("after close"));
    BOOST_CHECK(!fs::exists(cfg.jsonlFile));
}

BOOST_AUTO_TEST_CASE(recovers_from_a_rotation_abandoned_half_way)
{
    // Rotation shifts the text chain and the jsonl chain in turn, so a failure
    // between the two leaves the pair half-shifted: the text file has become .1
    // while the jsonl file is still the active one. Reopening does not repair
    // that — it appends to whatever is on disk and takes the sizes from there.
    OutputConfig cfg;
    cfg.name             = "main";
    cfg.textFile         = (dir / "syslog.log").string();
    cfg.jsonlFile        = (dir / "syslog.jsonl").string();
    cfg.maxSize          = 1; // every write rotates
    cfg.maxFiles         = 1; // so rotation deletes generation 1 rather than shifting it
    cfg.includeMalformed = true;

    // A non-empty directory where the rotated jsonl generation belongs: removing
    // it fails, which is what aborts the rotation part way through. Portable —
    // no filesystem removes a directory that has a file in it.
    const auto blocker = dir / "syslog.1.jsonl";
    fs::create_directories(blocker);
    std::ofstream(blocker / "occupied") << "x";

    LogFile lf(ioc, cfg, kTestRetry);

    writeSync(lf, messageWith("first"));
    BOOST_REQUIRE(fs::exists(cfg.textFile));
    BOOST_REQUIRE(fs::exists(cfg.jsonlFile));

    // Rotates: the text file moves to syslog.1.log, then the jsonl chain cannot
    // be shifted and the sink closes.
    writeSync(lf, messageWith("second"));
    BOOST_REQUIRE(fs::exists(dir / "syslog.1.log"));
    BOOST_REQUIRE(!fs::exists(cfg.textFile));
    BOOST_REQUIRE(readAll(cfg.jsonlFile).find("first") != std::string::npos);

    fs::remove_all(blocker);
    BOOST_REQUIRE(runUntilIdle(kRecoveryTimeout));

    // The reopened sink writes to both files again, and the jsonl record that
    // the aborted rotation left in the active file is carried into the chain by
    // the rotation that follows rather than lost. Its text counterpart is gone,
    // but for an ordinary reason: max_files = 1 keeps one generation, and this
    // write is the second rotation of that chain.
    writeSync(lf, messageWith("third"));
    BOOST_CHECK(readAll(cfg.textFile).find("third") != std::string::npos);
    BOOST_CHECK(readAll(cfg.jsonlFile).find("third") != std::string::npos);
    BOOST_CHECK(readAll(dir / "syslog.1.jsonl").find("first") != std::string::npos);

    lf.close();
    ioc.restart();
    ioc.poll();
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Filesystem failure handling ─────────────────────────────────────────────
//
// A storage problem must degrade the one sink that hit it, never the process.
// These need an unreadable directory, so they are POSIX-only and meaningless as
// root, where the permission bits are not enforced.

#ifndef _WIN32

BOOST_FIXTURE_TEST_SUITE(filesystem_failures, Fixture)

namespace
{

// True when the permission bits cannot be trusted to deny anything.
bool runningAsRoot()
{
    return ::geteuid() == 0;
}

// Denies all access to a directory and restores it on scope exit — including
// when a failed assertion unwinds, which would otherwise leave an unreadable
// directory behind for the next run to trip over.
class DeniedDirectory
{
public:
    explicit DeniedDirectory(fs::path dir) : m_dir(std::move(dir))
    {
        fs::permissions(m_dir, fs::perms::none);
    }

    ~DeniedDirectory()
    {
        std::error_code ec;
        fs::permissions(m_dir, fs::perms::owner_all, ec);
    }

    DeniedDirectory(const DeniedDirectory&)            = delete;
    DeniedDirectory& operator=(const DeniedDirectory&) = delete;

private:
    fs::path m_dir;
};

} // namespace

BOOST_AUTO_TEST_CASE(rotation_permission_denied_closes_sink_without_aborting)
{
    if (runningAsRoot())
    {
        BOOST_TEST_MESSAGE("skipped: permission bits do not deny root");
        return;
    }

    // maxSize 1 makes the second write rotate.
    const auto cfg = sinkConfig(dir / "syslog.jsonl", 1);
    LogFile lf(ioc, cfg);

    writeSync(lf, messageWith("first"));
    BOOST_REQUIRE(fs::exists(cfg.jsonlFile));

    // Deny everything in the directory, so the rotation probe fails. Before the
    // fix this threw filesystem_error out of the strand handler and aborted.
    {
        const DeniedDirectory denied(dir);
        writeSync(lf, messageWith("second"));
    }

    // Reaching here at all is the main assertion. The sink is closed, so a write
    // is dropped even though the directory is readable again: the retry runs on a
    // timer, and these tests only poll the io_context, so the default 30 s
    // interval cannot elapse inside one. Recovery has its own suite above.
    const auto contents = readAll(cfg.jsonlFile);
    writeSync(lf, messageWith("third"));
    BOOST_CHECK_EQUAL(readAll(cfg.jsonlFile), contents);
    BOOST_CHECK(contents.find("third") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(open_permission_denied_closes_sink)
{
    if (runningAsRoot())
    {
        BOOST_TEST_MESSAGE("skipped: permission bits do not deny root");
        return;
    }

    // The sink opens lazily on first write, so denying the directory up front
    // makes the very first write fail.
    const auto subdir = dir / "denied";
    fs::create_directories(subdir);
    const auto cfg = sinkConfig(subdir / "syslog.jsonl", 0);

    LogFile lf(ioc, cfg);
    {
        const DeniedDirectory denied(subdir);
        writeSync(lf, messageWith("never lands"));
    }

    BOOST_CHECK(!fs::exists(cfg.jsonlFile));
}

BOOST_AUTO_TEST_CASE(failed_sink_does_not_stop_a_healthy_one)
{
    if (runningAsRoot())
    {
        BOOST_TEST_MESSAGE("skipped: permission bits do not deny root");
        return;
    }

    const auto goodDir = dir / "good";
    const auto badDir  = dir / "bad";
    fs::create_directories(goodDir);
    fs::create_directories(badDir);

    const auto goodCfg = sinkConfig(goodDir / "syslog.jsonl", 0); // never rotates
    const auto badCfg  = sinkConfig(badDir / "syslog.jsonl", 1);  // rotates every write

    LogFile good(ioc, goodCfg);
    LogFile bad(ioc, badCfg);

    writeSync(good, messageWith("before"));
    writeSync(bad, messageWith("before"));

    {
        const DeniedDirectory denied(badDir);
        writeSync(bad, messageWith("kills the bad sink"));
    }

    // The healthy sink shares the io_context with the failed one and must be
    // entirely unaffected by it.
    writeSync(good, messageWith("after"));
    const auto contents = readAll(goodCfg.jsonlFile);
    BOOST_CHECK(contents.find("before") != std::string::npos);
    BOOST_CHECK(contents.find("after") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

#endif // !_WIN32
