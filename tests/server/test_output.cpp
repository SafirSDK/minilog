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

#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace minilog;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace
{

// Unique temp directory per test case.
struct Fixture
{
    fs::path dir;
    boost::asio::io_context ioc;

    Fixture()
    {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("minilog_test_" + std::to_string(++counter));
        fs::create_directories(dir);
    }

    ~Fixture() { fs::remove_all(dir); }

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
