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

#define BOOST_TEST_MODULE test_send
#include "send_options.hpp"
#include "syslog_format.hpp"

#include "parser/syslog_names.hpp"
#include "parser/syslog_parser.hpp"

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace minilog;
using namespace minilog::send;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace
{

SendOptions parse(std::vector<const char*> args)
{
    args.insert(args.begin(), "minilog-send");
    return parseSendOptions(static_cast<int>(args.size()), args.data());
}

// A fixed instant: 2026-09-03 07:05:09.000042 local, two hours east of UTC.
LocalTime fixedTime()
{
    LocalTime t;
    t.tm.tm_year       = 2026 - 1900;
    t.tm.tm_mon        = 8; // September
    t.tm.tm_mday       = 3;
    t.tm.tm_hour       = 7;
    t.tm.tm_min        = 5;
    t.tm.tm_sec        = 9;
    t.microseconds     = 42;
    t.utcOffsetSeconds = 2 * 3600;
    return t;
}

SyslogFields fullFields()
{
    SyslogFields f;
    f.facility  = 3; // daemon
    f.severity  = 4; // warning
    f.timestamp = "2026-09-03T07:05:09.000042+02:00";
    f.hostname  = "buildbox";
    f.app       = "backup";
    f.pid       = "4242";
    f.msgid     = "DONE";
    f.message   = "nightly backup finished";
    return f;
}

} // namespace

// ─── Option parsing ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(options)

BOOST_AUTO_TEST_CASE(defaults)
{
    const auto o = parse({"hello"});
    BOOST_TEST(o.host == "127.0.0.1");
    BOOST_TEST(o.port == 514);
    BOOST_TEST(o.facility == 1);
    BOOST_TEST(o.severity == 6);
    BOOST_TEST(o.app == "minilog-send");
    BOOST_TEST(!o.hostname.has_value());
    BOOST_TEST(!o.pid.has_value());
    BOOST_TEST(!o.msgid.has_value());
    BOOST_TEST(!o.rfc3164);
    BOOST_TEST(o.message == "hello");
    BOOST_TEST(!o.help);
    BOOST_TEST(!o.version);
}

BOOST_AUTO_TEST_CASE(every_flag_long_form)
{
    const auto o = parse({"--host",
                          "collector.example",
                          "--port",
                          "5514",
                          "--facility",
                          "local3",
                          "--severity",
                          "error",
                          "--app",
                          "deploy",
                          "--hostname",
                          "web01",
                          "--pid",
                          "77",
                          "--msgid",
                          "STEP3",
                          "--rfc3164",
                          "it",
                          "worked"});
    BOOST_TEST(o.host == "collector.example");
    BOOST_TEST(o.port == 5514);
    BOOST_TEST(o.facility == 19);
    BOOST_TEST(o.severity == 3);
    BOOST_TEST(o.app == "deploy");
    BOOST_TEST(*o.hostname == "web01");
    BOOST_TEST(*o.pid == "77");
    BOOST_TEST(*o.msgid == "STEP3");
    BOOST_TEST(o.rfc3164);
    BOOST_TEST(o.message == "it worked");
}

BOOST_AUTO_TEST_CASE(short_forms)
{
    const auto o = parse({"-f", "auth", "-s", "crit", "-a", "sshd", "msg"});
    BOOST_TEST(o.facility == 4);
    BOOST_TEST(o.severity == 2);
    BOOST_TEST(o.app == "sshd");
}

BOOST_AUTO_TEST_CASE(words_joined_with_single_spaces)
{
    // Quoting on the shell side is preserved as one word; separate words get
    // one space each, whatever spacing the shell collapsed.
    const auto o = parse({"a", "b  c", "d"});
    BOOST_TEST(o.message == "a b  c d");
}

BOOST_AUTO_TEST_CASE(no_words_means_stdin)
{
    const auto o = parse({"-s", "notice"});
    BOOST_TEST(o.message.empty());
}

BOOST_AUTO_TEST_CASE(double_dash_ends_options)
{
    const auto o = parse({"--", "-not", "--an-option"});
    BOOST_TEST(o.message == "-not --an-option");
}

BOOST_AUTO_TEST_CASE(numeric_facility_and_severity)
{
    const auto o = parse({"-f", "16", "-s", "0", "m"});
    BOOST_TEST(o.facility == 16);
    BOOST_TEST(o.severity == 0);
}

BOOST_AUTO_TEST_CASE(numeric_out_of_range_is_a_usage_error)
{
    BOOST_CHECK_THROW(parse({"-f", "24", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"-s", "8", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"-f", "9999999999999", "m"}), UsageError);
}

BOOST_AUTO_TEST_CASE(names_are_case_insensitive)
{
    const auto o = parse({"-f", "LOCAL0", "-s", "Warning", "m"});
    BOOST_TEST(o.facility == 16);
    BOOST_TEST(o.severity == 4);
}

BOOST_AUTO_TEST_CASE(every_facility_name_the_jsonl_writes_is_accepted)
{
    // The property that matters: whatever minilog will write for a facility,
    // minilog-send takes as input and maps back to the same number.
    for (std::size_t code = 0; code < kFacilityNames.size(); ++code)
    {
        const auto o = parse({"-f", kFacilityNames[code].data(), "m"});
        BOOST_TEST(o.facility == static_cast<int>(code), kFacilityNames[code]);
    }
    for (std::size_t code = 0; code < kSeverityNames.size(); ++code)
    {
        const auto o = parse({"-s", kSeverityNames[code].data(), "m"});
        BOOST_TEST(o.severity == static_cast<int>(code), kSeverityNames[code]);
    }
}

BOOST_AUTO_TEST_CASE(syslog3_severity_spellings)
{
    BOOST_TEST(parse({"-s", "emerg", "m"}).severity == 0);
    BOOST_TEST(parse({"-s", "panic", "m"}).severity == 0);
    BOOST_TEST(parse({"-s", "crit", "m"}).severity == 2);
    BOOST_TEST(parse({"-s", "err", "m"}).severity == 3);
    BOOST_TEST(parse({"-s", "warn", "m"}).severity == 4);
}

BOOST_AUTO_TEST_CASE(unknown_names_are_usage_errors)
{
    BOOST_CHECK_THROW(parse({"-f", "kitchen", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"-s", "loud", "m"}), UsageError);
}

BOOST_AUTO_TEST_CASE(port_range)
{
    BOOST_TEST(parse({"--port", "1", "m"}).port == 1);
    BOOST_TEST(parse({"--port", "65535", "m"}).port == 65535);
    BOOST_CHECK_THROW(parse({"--port", "0", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"--port", "65536", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"--port", "syslog", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"--port", "-1", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"--port", "", "m"}), UsageError);
    BOOST_CHECK_THROW(parse({"-f", "", "m"}), UsageError);
}

BOOST_AUTO_TEST_CASE(unknown_option_is_a_usage_error)
{
    BOOST_CHECK_THROW(parse({"--tcp", "m"}), UsageError);
}

BOOST_AUTO_TEST_CASE(missing_option_value_is_a_usage_error)
{
    BOOST_CHECK_THROW(parse({"m", "--port"}), UsageError);
}

BOOST_AUTO_TEST_CASE(help_and_version_short_circuit)
{
    // Nothing else on the line is validated once help or version is asked for.
    BOOST_TEST(parse({"--help", "--port", "bogus"}).help);
    BOOST_TEST(parse({"-h"}).help);
    BOOST_TEST(parse({"--version", "-f", "kitchen"}).version);
}

BOOST_AUTO_TEST_CASE(usage_text_documents_the_flags_and_the_udp_caveat)
{
    const auto text = usageText();
    for (const char* needle : {"--host",
                               "--port",
                               "--facility",
                               "--severity",
                               "--app",
                               "--hostname",
                               "--pid",
                               "--msgid",
                               "--rfc3164",
                               "stdin",
                               "does not mean the message arrived"})
    {
        BOOST_TEST(text.find(needle) != std::string::npos, needle);
    }
    // The positional catch-all is an implementation detail of the parser.
    BOOST_TEST(text.find("--message") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Formatting ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(format)

BOOST_AUTO_TEST_CASE(rfc5424_all_fields)
{
    BOOST_TEST(formatRfc5424(fullFields()) ==
               "<28>1 2026-09-03T07:05:09.000042+02:00 buildbox backup 4242 DONE - nightly "
               "backup finished");
}

BOOST_AUTO_TEST_CASE(rfc5424_nil_pid_and_msgid)
{
    auto f  = fullFields();
    f.pid   = std::nullopt;
    f.msgid = std::nullopt;
    BOOST_TEST(formatRfc5424(f) ==
               "<28>1 2026-09-03T07:05:09.000042+02:00 buildbox backup - - - nightly "
               "backup finished");
}

BOOST_AUTO_TEST_CASE(rfc3164_with_pid)
{
    auto f      = fullFields();
    f.timestamp = "Sep  3 07:05:09";
    BOOST_TEST(formatRfc3164(f) ==
               "<28>Sep  3 07:05:09 buildbox backup[4242]: nightly backup finished");
}

BOOST_AUTO_TEST_CASE(rfc3164_without_pid)
{
    auto f      = fullFields();
    f.timestamp = "Sep  3 07:05:09";
    f.pid       = std::nullopt;
    BOOST_TEST(formatRfc3164(f) == "<28>Sep  3 07:05:09 buildbox backup: nightly backup finished");
}

BOOST_AUTO_TEST_CASE(pri_is_facility_times_eight_plus_severity)
{
    auto f     = fullFields();
    f.facility = 0;
    f.severity = 0;
    BOOST_TEST(formatRfc5424(f).substr(0, 3) == "<0>");
    f.facility = 23;
    f.severity = 7;
    BOOST_TEST(formatRfc5424(f).substr(0, 5) == "<191>");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Timestamps ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(timestamps)

BOOST_AUTO_TEST_CASE(rfc5424_positive_offset)
{
    BOOST_TEST(rfc5424Timestamp(fixedTime()) == "2026-09-03T07:05:09.000042+02:00");
}

BOOST_AUTO_TEST_CASE(rfc5424_negative_offset_with_minutes)
{
    auto t             = fixedTime();
    t.utcOffsetSeconds = -(9 * 3600 + 30 * 60);
    BOOST_TEST(rfc5424Timestamp(t) == "2026-09-03T07:05:09.000042-09:30");
}

BOOST_AUTO_TEST_CASE(rfc5424_utc_is_plus_zero)
{
    auto t             = fixedTime();
    t.utcOffsetSeconds = 0;
    BOOST_TEST(rfc5424Timestamp(t) == "2026-09-03T07:05:09.000042+00:00");
}

BOOST_AUTO_TEST_CASE(rfc3164_single_digit_day_is_space_padded)
{
    BOOST_TEST(rfc3164Timestamp(fixedTime()) == "Sep  3 07:05:09");
}

BOOST_AUTO_TEST_CASE(rfc3164_two_digit_day)
{
    auto t       = fixedTime();
    t.tm.tm_mday = 23;
    t.tm.tm_mon  = 11;
    t.tm.tm_hour = 23;
    BOOST_TEST(rfc3164Timestamp(t) == "Dec 23 23:05:09");
}

BOOST_AUTO_TEST_CASE(local_now_produces_parseable_timestamps)
{
    // The clock and zone cannot be asserted on, but both formats must at least
    // come out well-formed from the real thing.
    const auto now = localNow();
    BOOST_TEST(now.microseconds >= 0);
    BOOST_TEST(now.microseconds < 1000000);
    const auto ts = rfc5424Timestamp(now);
    BOOST_TEST(ts.size() == std::string("2026-09-03T07:05:09.000042+02:00").size(), ts);
    BOOST_TEST(rfc3164Timestamp(now).size() == std::string("Sep  3 07:05:09").size());
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Validation ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(validation)

BOOST_AUTO_TEST_CASE(full_fields_pass_both_formats)
{
    BOOST_CHECK_NO_THROW(validateFields(fullFields(), false));
    auto f  = fullFields();
    f.msgid = std::nullopt;
    BOOST_CHECK_NO_THROW(validateFields(f, true));
}

BOOST_AUTO_TEST_CASE(space_in_a_header_field_is_rejected)
{
    for (const bool rfc3164 : {false, true})
    {
        auto f     = fullFields();
        f.msgid    = std::nullopt;
        f.hostname = "two words";
        BOOST_CHECK_THROW(validateFields(f, rfc3164), std::runtime_error);
        f       = fullFields();
        f.msgid = std::nullopt;
        f.app   = "my app";
        BOOST_CHECK_THROW(validateFields(f, rfc3164), std::runtime_error);
        f       = fullFields();
        f.msgid = std::nullopt;
        f.pid   = "1 2";
        BOOST_CHECK_THROW(validateFields(f, rfc3164), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(empty_header_field_is_rejected)
{
    auto f = fullFields();
    f.app  = "";
    BOOST_CHECK_THROW(validateFields(f, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(non_ascii_header_field_is_rejected)
{
    auto f     = fullFields();
    f.hostname = "h\xc3\xb6st";
    BOOST_CHECK_THROW(validateFields(f, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(msgid_with_a_space_is_rejected)
{
    auto f  = fullFields();
    f.msgid = "a b";
    BOOST_CHECK_THROW(validateFields(f, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(msgid_has_no_place_in_rfc3164)
{
    BOOST_CHECK_THROW(validateFields(fullFields(), true), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rfc3164_tag_delimiters_are_rejected_in_app_and_pid)
{
    for (const char* app : {"a[b", "a]b", "a:b"})
    {
        auto f  = fullFields();
        f.msgid = std::nullopt;
        f.app   = app;
        BOOST_CHECK_THROW(validateFields(f, true), std::runtime_error);
        // The same characters are fine in RFC 5424, where the header is
        // space-delimited only.
        BOOST_CHECK_NO_THROW(validateFields(f, false));
    }
    auto f  = fullFields();
    f.msgid = std::nullopt;
    f.pid   = "1]2";
    BOOST_CHECK_THROW(validateFields(f, true), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(facility_and_severity_ranges)
{
    auto f     = fullFields();
    f.facility = 24;
    BOOST_CHECK_THROW(validateFields(f, false), std::runtime_error);
    f          = fullFields();
    f.severity = -1;
    BOOST_CHECK_THROW(validateFields(f, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(empty_message_is_rejected)
{
    auto f    = fullFields();
    f.message = "";
    BOOST_CHECK_THROW(validateFields(f, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(message_may_contain_anything)
{
    // The body is the receiver's problem: minilog escapes control characters
    // in the text sink and replaces bad UTF-8 in the JSONL.
    auto f    = fullFields();
    f.message = "tabs\tand\x01 control and \xff bytes and spaces  ";
    BOOST_CHECK_NO_THROW(validateFields(f, false));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── stdin splitting ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(split_lines)

BOOST_AUTO_TEST_CASE(lf_with_trailing_newline)
{
    const std::vector<std::string> expected = {"one", "two"};
    BOOST_TEST(splitLines("one\ntwo\n") == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(missing_trailing_newline)
{
    const std::vector<std::string> expected = {"one", "two"};
    BOOST_TEST(splitLines("one\ntwo") == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(crlf)
{
    const std::vector<std::string> expected = {"one", "two"};
    BOOST_TEST(splitLines("one\r\ntwo\r\n") == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(empty_lines_are_dropped)
{
    const std::vector<std::string> expected = {"one", "two"};
    BOOST_TEST(splitLines("\n\none\n\r\n\ntwo\n\n") == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(empty_input)
{
    BOOST_TEST(splitLines("").empty());
    BOOST_TEST(splitLines("\n\r\n").empty());
}

BOOST_AUTO_TEST_CASE(interior_whitespace_is_kept)
{
    const std::vector<std::string> expected = {"  indented  ", "a\tb"};
    BOOST_TEST(splitLines("  indented  \na\tb\n") == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Round trip through the server's parser ──────────────────────────────────

BOOST_AUTO_TEST_SUITE(round_trip)

BOOST_AUTO_TEST_CASE(rfc5424_every_field_comes_back)
{
    const auto f = fullFields();
    const auto m = parseSyslog(formatRfc5424(f));
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.facility == f.facility);
    BOOST_TEST(*m.severity == f.severity);
    BOOST_TEST(*m.facilityName == "daemon");
    BOOST_TEST(*m.severityName == "WARNING");
    BOOST_TEST(*m.timestamp == f.timestamp);
    BOOST_TEST(*m.hostname == f.hostname);
    BOOST_TEST(*m.appName == f.app);
    BOOST_TEST(*m.procId == *f.pid);
    BOOST_TEST(*m.msgId == *f.msgid);
    // The structured-data "-" stays a prefix of the message; that is how minilog
    // has always stored RFC 5424 and the viewers know it.
    BOOST_TEST(m.message == "- " + f.message);
}

BOOST_AUTO_TEST_CASE(rfc5424_nil_fields_come_back_absent)
{
    auto f       = fullFields();
    f.pid        = std::nullopt;
    f.msgid      = std::nullopt;
    const auto m = parseSyslog(formatRfc5424(f));
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(!m.procId.has_value());
    BOOST_TEST(!m.msgId.has_value());
}

BOOST_AUTO_TEST_CASE(rfc3164_every_field_comes_back)
{
    auto f       = fullFields();
    f.msgid      = std::nullopt;
    f.timestamp  = rfc3164Timestamp(fixedTime());
    const auto m = parseSyslog(formatRfc3164(f));
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.facility == f.facility);
    BOOST_TEST(*m.severity == f.severity);
    BOOST_TEST(*m.timestamp == "Sep  3 07:05:09");
    BOOST_TEST(*m.hostname == f.hostname);
    BOOST_TEST(*m.appName == f.app);
    BOOST_TEST(*m.procId == *f.pid);
    BOOST_TEST(!m.msgId.has_value());
    BOOST_TEST(m.message == f.message);
}

BOOST_AUTO_TEST_CASE(rfc3164_without_pid)
{
    auto f       = fullFields();
    f.msgid      = std::nullopt;
    f.pid        = std::nullopt;
    f.timestamp  = rfc3164Timestamp(fixedTime());
    const auto m = parseSyslog(formatRfc3164(f));
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.appName == f.app);
    BOOST_TEST(!m.procId.has_value());
    BOOST_TEST(m.message == f.message);
}

BOOST_AUTO_TEST_CASE(real_clock_parses_in_both_formats)
{
    auto f      = fullFields();
    f.timestamp = rfc5424Timestamp(localNow());
    BOOST_TEST((parseSyslog(formatRfc5424(f)).protocol == Protocol::RFC5424));
    f.msgid     = std::nullopt;
    f.timestamp = rfc3164Timestamp(localNow());
    BOOST_TEST((parseSyslog(formatRfc3164(f)).protocol == Protocol::RFC3164));
}

BOOST_AUTO_TEST_CASE(every_named_facility_round_trips_to_its_name)
{
    // What the tool accepts by name, minilog writes back under that name.
    for (std::size_t code = 0; code < kFacilityNames.size(); ++code)
    {
        auto f       = fullFields();
        f.facility   = static_cast<int>(code);
        const auto m = parseSyslog(formatRfc5424(f));
        BOOST_TEST(*m.facilityName == kFacilityNames[code]);
    }
}

BOOST_AUTO_TEST_SUITE_END()
