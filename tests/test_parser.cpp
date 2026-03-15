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

#define BOOST_TEST_MODULE test_parser
#include "parser/syslog_parser.hpp"

#include <boost/test/unit_test.hpp>

using namespace minilog;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static SyslogMessage parse(std::string_view s)
{
    return parseSyslog(s);
}

// ─── RFC5424 ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc5424)

BOOST_AUTO_TEST_CASE(basic)
{
    const auto m = parse("<34>1 2026-03-12T14:30:22Z mymachine su 123 ID47 - message text");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.pri == 34);
    BOOST_TEST(*m.facility == 4); // 34/8 = 4 (auth)
    BOOST_TEST(*m.severity == 2); // 34%8 = 2 (critical)
    BOOST_TEST(*m.facilityName == "auth");
    BOOST_TEST(*m.severityName == "CRITICAL");
    BOOST_TEST(*m.version == 1);
    BOOST_TEST(*m.timestamp == "2026-03-12T14:30:22Z");
    BOOST_TEST(*m.hostname == "mymachine");
    BOOST_TEST(*m.appName == "su");
    BOOST_TEST(*m.procId == "123");
    BOOST_TEST(*m.msgId == "ID47");
    BOOST_TEST(m.message == "- message text");
}

BOOST_AUTO_TEST_CASE(all_nil_fields)
{
    const auto m = parse("<13>1 - - - - - - nil message");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(!m.timestamp.has_value());
    BOOST_TEST(!m.hostname.has_value());
    BOOST_TEST(!m.appName.has_value());
    BOOST_TEST(!m.procId.has_value());
    BOOST_TEST(!m.msgId.has_value());
    BOOST_TEST(m.message == "- nil message");
}

BOOST_AUTO_TEST_CASE(structured_data_in_message)
{
    // Structured data is NOT stripped — it stays verbatim in message
    const auto m = parse("<165>1 2026-01-01T00:00:00Z host app - - [sd1 k=\"v\"] body");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "[sd1 k=\"v\"] body");
}

BOOST_AUTO_TEST_CASE(multiple_structured_data_elements)
{
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - [sd1 x=\"1\"][sd2 y=\"2\"] msg");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "[sd1 x=\"1\"][sd2 y=\"2\"] msg");
}

BOOST_AUTO_TEST_CASE(nil_sd_with_msg)
{
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - - actual message");
    BOOST_TEST(m.message == "- actual message");
}

BOOST_AUTO_TEST_CASE(version_not_1_falls_through)
{
    // Version 2 is not recognised; falls through to RFC3164 then UNKNOWN
    const auto m = parse("<34>2 2026-03-12T14:30:22Z mymachine su - - - msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(pri_zero)
{
    const auto m = parse("<0>1 2026-01-01T00:00:00Z h a - - - m");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.pri == 0);
    BOOST_TEST(*m.facility == 0);
    BOOST_TEST(*m.severity == 0);
}

BOOST_AUTO_TEST_CASE(pri_191_max_valid)
{
    const auto m = parse("<191>1 2026-01-01T00:00:00Z h a - - - m");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.pri == 191);
    BOOST_TEST(*m.facility == 23);
    BOOST_TEST(*m.severity == 7);
}

BOOST_AUTO_TEST_CASE(pri_192_invalid)
{
    const auto m = parse("<192>1 2026-01-01T00:00:00Z h a - - - m");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(empty_message_body)
{
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - -");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "-");
}

BOOST_AUTO_TEST_CASE(trailing_crlf_stripped)
{
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - - msg\r\n");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "- msg");
}

BOOST_AUTO_TEST_CASE(embedded_crlf_preserved)
{
    // CRLF in the middle of the message is NOT stripped — only trailing CRLF is.
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - - before\r\nafter");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "- before\r\nafter");
}

BOOST_AUTO_TEST_CASE(bom_prefix_no_crash)
{
    // UTF-8 BOM (\xEF\xBB\xBF) at the start of the MSG field is kept verbatim.
    const std::string s = "<34>1 2026-01-01T00:00:00Z h a - - - \xEF\xBB\xBFhello";
    BOOST_CHECK_NO_THROW(parse(s));
    const auto m = parse(s);
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message.find('\xEF') != std::string::npos);
}

BOOST_AUTO_TEST_CASE(very_long_message)
{
    const std::string body(1000, 'x');
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - - " + body);
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    // message = "- " + 1000 x's (SD nil + space + body)
    BOOST_TEST(m.message.size() == 1002u);
}

BOOST_AUTO_TEST_CASE(message_at_max_udp_size)
{
    // 65507 = max IPv4 UDP payload (65535 - 20 IP header - 8 UDP header).
    // "<0>1 - - - - - " = 15 bytes; fill the rest with 'x'.
    const std::string hdr     = "<0>1 - - - - - ";
    const std::string payload = hdr + std::string(65507 - hdr.size(), 'x');
    BOOST_REQUIRE_EQUAL(payload.size(), 65507u);
    BOOST_CHECK_NO_THROW(parse(payload));
    BOOST_TEST((parse(payload).protocol == Protocol::RFC5424));
}

BOOST_AUTO_TEST_CASE(raw_field_preserved)
{
    const std::string s = "<34>1 2026-01-01T00:00:00Z h a - - - msg";
    const auto m        = parse(s);
    BOOST_TEST(m.raw == s);
}

BOOST_AUTO_TEST_CASE(too_few_spaces_falls_through)
{
    const auto m = parse("<34>1 2026-01-01T00:00:00Z");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── RFC3164 ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc3164)

BOOST_AUTO_TEST_CASE(basic)
{
    const auto m = parse("<34>Jan 12 15:04:05 mymachine su[123]: this is the message");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.pri == 34);
    BOOST_TEST(*m.facility == 4);
    BOOST_TEST(*m.severity == 2);
    BOOST_TEST(*m.timestamp == "Jan 12 15:04:05");
    BOOST_TEST(*m.hostname == "mymachine");
    BOOST_TEST(*m.appName == "su");
    BOOST_TEST(*m.procId == "123");
    BOOST_TEST(m.message == "this is the message");
}

BOOST_AUTO_TEST_CASE(single_digit_day_space_padded)
{
    const auto m = parse("<34>Jan  5 09:00:00 host app: body");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.timestamp == "Jan  5 09:00:00");
}

BOOST_AUTO_TEST_CASE(no_pid)
{
    const auto m = parse("<13>Feb 28 10:20:30 myhost myapp: message body");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.appName == "myapp");
    BOOST_TEST(!m.procId.has_value());
    BOOST_TEST(m.message == "message body");
}

BOOST_AUTO_TEST_CASE(no_colon_in_tag)
{
    // Tag without colon — first word is appName, remainder is message
    const auto m = parse("<13>Feb 28 10:20:30 myhost myapp rest of message");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.appName == "myapp");
    BOOST_TEST(m.message == "rest of message");
}

BOOST_AUTO_TEST_CASE(empty_message_after_tag)
{
    const auto m = parse("<13>Mar  1 00:00:00 host app:");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.appName == "app");
    BOOST_TEST(m.message == "");
}

BOOST_AUTO_TEST_CASE(pri_zero)
{
    const auto m = parse("<0>Jan  1 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.pri == 0);
    BOOST_TEST(*m.facility == 0);
    BOOST_TEST(*m.severity == 0);
}

BOOST_AUTO_TEST_CASE(pri_191_max_valid)
{
    const auto m = parse("<191>Jan  1 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.pri == 191);
    BOOST_TEST(*m.facility == 23);
    BOOST_TEST(*m.severity == 7);
}

BOOST_AUTO_TEST_CASE(pri_192_invalid)
{
    const auto m = parse("<192>Jan  1 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(no_pri_falls_through)
{
    const auto m = parse("Jan 12 15:04:05 host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(trailing_crlf_stripped)
{
    const auto m = parse("<13>Jan 12 15:04:05 host app: msg\r\n");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(m.message == "msg");
}

BOOST_AUTO_TEST_CASE(embedded_crlf_preserved)
{
    // CRLF in the middle of the message is NOT stripped.
    const auto m = parse("<13>Jan  1 00:00:00 h app: before\r\nafter");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(m.message == "before\r\nafter");
}

BOOST_AUTO_TEST_CASE(very_long_message)
{
    const std::string body(1000, 'x');
    const auto m = parse("<13>Jan  1 00:00:00 h app: " + body);
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(m.message == body);
}

BOOST_AUTO_TEST_CASE(facility_names_mapped)
{
    // PRI 30: facility=3 (daemon), severity=6 (info)
    const auto m = parse("<30>Jan  1 00:00:00 h a: m");
    BOOST_TEST(*m.facilityName == "daemon");
    BOOST_TEST(*m.severityName == "INFO");
}

BOOST_AUTO_TEST_CASE(all_months_accepted)
{
    const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (const char* mon : months)
    {
        const std::string s = std::string("<13>") + mon + "  1 00:00:00 h a: m";
        const auto msg      = parseSyslog(s);
        BOOST_TEST_MESSAGE("month=" << mon);
        BOOST_TEST((msg.protocol == Protocol::RFC3164));
    }
}

BOOST_AUTO_TEST_CASE(bad_month_falls_through)
{
    const auto m = parse("<13>Xyz 12 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(bad_time_format_falls_through)
{
    const auto m = parse("<13>Jan 12 15:04:XX host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(out_of_range_day_falls_through)
{
    // Day 0 is invalid
    BOOST_TEST((parse("<13>Jan  0 00:00:00 host app: msg").protocol == Protocol::Unknown));
    // Day 32 is invalid
    BOOST_TEST((parse("<13>Jan 32 00:00:00 host app: msg").protocol == Protocol::Unknown));
    // Day 99 is invalid (fits in 2 digits but out of range)
    BOOST_TEST((parse("<13>Jan 99 00:00:00 host app: msg").protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(out_of_range_time_falls_through)
{
    // Hour 24 is invalid
    BOOST_TEST((parse("<13>Jan 12 24:00:00 host app: msg").protocol == Protocol::Unknown));
    // Minute 60 is invalid
    BOOST_TEST((parse("<13>Jan 12 00:60:00 host app: msg").protocol == Protocol::Unknown));
    // Second 60 is invalid
    BOOST_TEST((parse("<13>Jan 12 00:00:60 host app: msg").protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(raw_field_preserved)
{
    const std::string s = "<34>Jan 12 15:04:05 h su[1]: msg";
    const auto m        = parse(s);
    BOOST_TEST(m.raw == s);
}

BOOST_AUTO_TEST_CASE(hostname_only_no_tag_body)
{
    const auto m = parse("<13>Jan 12 15:04:05 myhost");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.hostname == "myhost");
    BOOST_TEST(m.message == "");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Unknown / malformed ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(unknown)

BOOST_AUTO_TEST_CASE(empty_input)
{
    const auto m = parse("");
    BOOST_TEST((m.protocol == Protocol::Unknown));
    BOOST_TEST(m.raw == "");
    BOOST_TEST(m.message == "");
}

BOOST_AUTO_TEST_CASE(plain_text_no_pri)
{
    const auto m = parse("hello world");
    BOOST_TEST((m.protocol == Protocol::Unknown));
    BOOST_TEST(m.message == "hello world");
}

BOOST_AUTO_TEST_CASE(pri_only_no_rest)
{
    const auto m = parse("<13>");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(random_binary_no_crash)
{
    const std::string bin(64, '\xff');
    BOOST_CHECK_NO_THROW(parse(bin));
    BOOST_TEST((parse(bin).protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(null_bytes_no_crash)
{
    std::string s = "<13>";
    s += '\x00';
    s += '\x01';
    s += '\x02';
    BOOST_CHECK_NO_THROW(parse(s));
}

BOOST_AUTO_TEST_CASE(only_cr_lf)
{
    const auto m = parse("\r\n");
    BOOST_TEST((m.protocol == Protocol::Unknown));
    BOOST_TEST(m.raw == "");
    BOOST_TEST(m.message == "");
}

BOOST_AUTO_TEST_CASE(malformed_pri_no_close)
{
    const auto m = parse("<13 no close");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(non_utf8_bytes_no_crash)
{
    // Latin-1 / arbitrary high bytes in a valid RFC3164 message body.
    const std::string s = "<13>Jan  1 00:00:00 h app: \x80\x81\x82\x83";
    BOOST_CHECK_NO_THROW(parse(s));
    const auto m = parse(s);
    // Parser works byte-level; result may be RFC3164 or Unknown, but must not crash.
    BOOST_TEST((m.protocol == Protocol::RFC3164 || m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Real-world samples ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(real_world)

BOOST_AUTO_TEST_CASE(linux_kernel)
{
    const auto m = parse("<4>Jan  1 00:00:04 myhost kernel: Oops: general protection fault");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.appName == "kernel");
    BOOST_TEST(m.message == "Oops: general protection fault");
}

BOOST_AUTO_TEST_CASE(openssh)
{
    const auto m = parse("<38>Mar 12 14:22:01 server sshd[12345]: Accepted publickey for lars");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.appName == "sshd");
    BOOST_TEST(*m.procId == "12345");
    BOOST_TEST(m.message == "Accepted publickey for lars");
}

BOOST_AUTO_TEST_CASE(rfc5424_with_structured_data)
{
    const auto m = parse("<165>1 2026-03-12T14:30:22.123456Z mymachine evntslog 556 ID47"
                         " [exampleSDID@32473 iut=\"3\" eventSource=\"Application\"] BOMAn event");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.appName == "evntslog");
    BOOST_TEST(*m.procId == "556");
    BOOST_TEST(*m.msgId == "ID47");
    BOOST_TEST(m.message.find("[exampleSDID@32473") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(cisco_ios_style)
{
    const auto m =
        parse("<189>Jan  1 00:00:00 192.168.1.1 %SYS-5-CONFIG_I: Configured from console");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.hostname == "192.168.1.1");
    BOOST_TEST(m.appName.has_value());
    BOOST_TEST(m.message == "Configured from console");
}

BOOST_AUTO_TEST_CASE(juniper)
{
    // Juniper JunOS syslog in standard RFC3164 format.
    const auto m =
        parse("<189>Jan  1 00:00:00 router.example.com %BGP-5-ADJCHANGE: neighbor 10.0.0.1 Up");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.hostname == "router.example.com");
    BOOST_TEST(*m.appName == "%BGP-5-ADJCHANGE");
    BOOST_TEST(m.message == "neighbor 10.0.0.1 Up");
}

BOOST_AUTO_TEST_CASE(windows_event_log)
{
    // Windows Event Log forwarded via syslog in RFC3164 format.
    const auto m =
        parse("<14>Mar 15 12:00:00 WIN-SERVER MSWinEventLog[1]: Security 4624 Successful Logon");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.hostname == "WIN-SERVER");
    BOOST_TEST(*m.appName == "MSWinEventLog");
    BOOST_TEST(*m.procId == "1");
    BOOST_TEST(m.message == "Security 4624 Successful Logon");
}

BOOST_AUTO_TEST_SUITE_END()
