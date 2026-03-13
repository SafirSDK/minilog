#define BOOST_TEST_MODULE test_parser
#include <boost/test/unit_test.hpp>
#include "parser/syslog_parser.hpp"

using namespace minilog;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static SyslogMessage parse(std::string_view s) { return parse_syslog(s); }

// ─── RFC5424 ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc5424)

BOOST_AUTO_TEST_CASE(basic) {
    const auto m = parse("<34>1 2026-03-12T14:30:22Z mymachine su 123 ID47 - message text");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.pri      == 34);
    BOOST_TEST(*m.facility == 4);   // 34/8 = 4 (auth)
    BOOST_TEST(*m.severity == 2);   // 34%8 = 2 (critical)
    BOOST_TEST(*m.facility_name == "auth");
    BOOST_TEST(*m.severity_name == "CRITICAL");
    BOOST_TEST(*m.version   == 1);
    BOOST_TEST(*m.timestamp == "2026-03-12T14:30:22Z");
    BOOST_TEST(*m.hostname  == "mymachine");
    BOOST_TEST(*m.app_name  == "su");
    BOOST_TEST(*m.proc_id   == "123");
    BOOST_TEST(*m.msg_id    == "ID47");
    BOOST_TEST(m.message    == "- message text");
}

BOOST_AUTO_TEST_CASE(all_nil_fields) {
    const auto m = parse("<13>1 - - - - - - nil message");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(!m.timestamp.has_value());
    BOOST_TEST(!m.hostname.has_value());
    BOOST_TEST(!m.app_name.has_value());
    BOOST_TEST(!m.proc_id.has_value());
    BOOST_TEST(!m.msg_id.has_value());
    BOOST_TEST(m.message == "- nil message");
}

BOOST_AUTO_TEST_CASE(structured_data_in_message) {
    // Structured data is NOT stripped — it stays verbatim in message
    const auto m = parse("<165>1 2026-01-01T00:00:00Z host app - - [sd1 k=\"v\"] body");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "[sd1 k=\"v\"] body");
}

BOOST_AUTO_TEST_CASE(multiple_structured_data_elements) {
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - [sd1 x=\"1\"][sd2 y=\"2\"] msg");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "[sd1 x=\"1\"][sd2 y=\"2\"] msg");
}

BOOST_AUTO_TEST_CASE(nil_sd_with_msg) {
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - - actual message");
    BOOST_TEST(m.message == "- actual message");
}

BOOST_AUTO_TEST_CASE(version_not_1_falls_through) {
    // Version 2 is not recognised; falls through to RFC3164 then UNKNOWN
    const auto m = parse("<34>2 2026-03-12T14:30:22Z mymachine su - - - msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(pri_zero) {
    const auto m = parse("<0>1 2026-01-01T00:00:00Z h a - - - m");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.pri      == 0);
    BOOST_TEST(*m.facility == 0);
    BOOST_TEST(*m.severity == 0);
}

BOOST_AUTO_TEST_CASE(pri_191_max_valid) {
    const auto m = parse("<191>1 2026-01-01T00:00:00Z h a - - - m");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.pri      == 191);
    BOOST_TEST(*m.facility == 23);
    BOOST_TEST(*m.severity == 7);
}

BOOST_AUTO_TEST_CASE(pri_192_invalid) {
    const auto m = parse("<192>1 2026-01-01T00:00:00Z h a - - - m");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(empty_message_body) {
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - -");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "-");
}

BOOST_AUTO_TEST_CASE(trailing_crlf_stripped) {
    const auto m = parse("<34>1 2026-01-01T00:00:00Z h a - - - msg\r\n");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(m.message == "- msg");
}

BOOST_AUTO_TEST_CASE(raw_field_preserved) {
    const std::string s = "<34>1 2026-01-01T00:00:00Z h a - - - msg";
    const auto m = parse(s);
    BOOST_TEST(m.raw == s);
}

BOOST_AUTO_TEST_CASE(too_few_spaces_falls_through) {
    const auto m = parse("<34>1 2026-01-01T00:00:00Z");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── RFC3164 ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc3164)

BOOST_AUTO_TEST_CASE(basic) {
    const auto m = parse("<34>Jan 12 15:04:05 mymachine su[123]: this is the message");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.pri      == 34);
    BOOST_TEST(*m.facility == 4);
    BOOST_TEST(*m.severity == 2);
    BOOST_TEST(*m.timestamp == "Jan 12 15:04:05");
    BOOST_TEST(*m.hostname  == "mymachine");
    BOOST_TEST(*m.app_name  == "su");
    BOOST_TEST(*m.proc_id   == "123");
    BOOST_TEST(m.message    == "this is the message");
}

BOOST_AUTO_TEST_CASE(single_digit_day_space_padded) {
    const auto m = parse("<34>Jan  5 09:00:00 host app: body");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.timestamp == "Jan  5 09:00:00");
}

BOOST_AUTO_TEST_CASE(no_pid) {
    const auto m = parse("<13>Feb 28 10:20:30 myhost myapp: message body");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.app_name == "myapp");
    BOOST_TEST(!m.proc_id.has_value());
    BOOST_TEST(m.message   == "message body");
}

BOOST_AUTO_TEST_CASE(no_colon_in_tag) {
    // Tag without colon — first word is app_name, remainder is message
    const auto m = parse("<13>Feb 28 10:20:30 myhost myapp rest of message");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.app_name == "myapp");
    BOOST_TEST(m.message   == "rest of message");
}

BOOST_AUTO_TEST_CASE(empty_message_after_tag) {
    const auto m = parse("<13>Mar  1 00:00:00 host app:");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.app_name == "app");
    BOOST_TEST(m.message   == "");
}

BOOST_AUTO_TEST_CASE(pri_zero) {
    const auto m = parse("<0>Jan  1 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.pri      == 0);
    BOOST_TEST(*m.facility == 0);
    BOOST_TEST(*m.severity == 0);
}

BOOST_AUTO_TEST_CASE(pri_191_max_valid) {
    const auto m = parse("<191>Jan  1 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.pri      == 191);
    BOOST_TEST(*m.facility == 23);
    BOOST_TEST(*m.severity == 7);
}

BOOST_AUTO_TEST_CASE(pri_192_invalid) {
    const auto m = parse("<192>Jan  1 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(no_pri_falls_through) {
    const auto m = parse("Jan 12 15:04:05 host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(trailing_crlf_stripped) {
    const auto m = parse("<13>Jan 12 15:04:05 host app: msg\r\n");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(m.message == "msg");
}

BOOST_AUTO_TEST_CASE(facility_names_mapped) {
    // PRI 30: facility=3 (daemon), severity=6 (info)
    const auto m = parse("<30>Jan  1 00:00:00 h a: m");
    BOOST_TEST(*m.facility_name == "daemon");
    BOOST_TEST(*m.severity_name == "INFO");
}

BOOST_AUTO_TEST_CASE(all_months_accepted) {
    const char* months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    for (const char* mon : months) {
        const std::string s = std::string("<13>") + mon + "  1 00:00:00 h a: m";
        const auto msg = parse(s);
        BOOST_TEST_MESSAGE("month=" << mon);
        BOOST_TEST((msg.protocol == Protocol::RFC3164));
    }
}

BOOST_AUTO_TEST_CASE(bad_month_falls_through) {
    const auto m = parse("<13>Xyz 12 00:00:00 host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(bad_time_format_falls_through) {
    const auto m = parse("<13>Jan 12 15:04:XX host app: msg");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(raw_field_preserved) {
    const std::string s = "<34>Jan 12 15:04:05 h su[1]: msg";
    const auto m = parse(s);
    BOOST_TEST(m.raw == s);
}

BOOST_AUTO_TEST_CASE(hostname_only_no_tag_body) {
    const auto m = parse("<13>Jan 12 15:04:05 myhost");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.hostname == "myhost");
    BOOST_TEST(m.message   == "");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Unknown / malformed ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(unknown)

BOOST_AUTO_TEST_CASE(empty_input) {
    const auto m = parse("");
    BOOST_TEST((m.protocol == Protocol::Unknown));
    BOOST_TEST(m.raw     == "");
    BOOST_TEST(m.message == "");
}

BOOST_AUTO_TEST_CASE(plain_text_no_pri) {
    const auto m = parse("hello world");
    BOOST_TEST((m.protocol == Protocol::Unknown));
    BOOST_TEST(m.message == "hello world");
}

BOOST_AUTO_TEST_CASE(pri_only_no_rest) {
    const auto m = parse("<13>");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(random_binary_no_crash) {
    const std::string bin(64, '\xff');
    BOOST_CHECK_NO_THROW(parse(bin));
    BOOST_TEST((parse(bin).protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_CASE(null_bytes_no_crash) {
    std::string s = "<13>";
    s += '\x00'; s += '\x01'; s += '\x02';
    BOOST_CHECK_NO_THROW(parse(s));
}

BOOST_AUTO_TEST_CASE(only_cr_lf) {
    const auto m = parse("\r\n");
    BOOST_TEST((m.protocol == Protocol::Unknown));
    BOOST_TEST(m.raw     == "");
    BOOST_TEST(m.message == "");
}

BOOST_AUTO_TEST_CASE(malformed_pri_no_close) {
    const auto m = parse("<13 no close");
    BOOST_TEST((m.protocol == Protocol::Unknown));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Real-world samples ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(real_world)

BOOST_AUTO_TEST_CASE(linux_kernel) {
    const auto m = parse("<4>Jan  1 00:00:04 myhost kernel: Oops: general protection fault");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.app_name == "kernel");
    BOOST_TEST(m.message   == "Oops: general protection fault");
}

BOOST_AUTO_TEST_CASE(openssh) {
    const auto m = parse("<38>Mar 12 14:22:01 server sshd[12345]: Accepted publickey for lars");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.app_name == "sshd");
    BOOST_TEST(*m.proc_id  == "12345");
    BOOST_TEST(m.message   == "Accepted publickey for lars");
}

BOOST_AUTO_TEST_CASE(rfc5424_with_structured_data) {
    const auto m = parse(
        "<165>1 2026-03-12T14:30:22.123456Z mymachine evntslog 556 ID47"
        " [exampleSDID@32473 iut=\"3\" eventSource=\"Application\"] BOMAn event");
    BOOST_TEST((m.protocol == Protocol::RFC5424));
    BOOST_TEST(*m.app_name == "evntslog");
    BOOST_TEST(*m.proc_id  == "556");
    BOOST_TEST(*m.msg_id   == "ID47");
    BOOST_TEST(m.message.find("[exampleSDID@32473") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(cisco_ios_style) {
    const auto m = parse("<189>Jan  1 00:00:00 192.168.1.1 %SYS-5-CONFIG_I: Configured from console");
    BOOST_TEST((m.protocol == Protocol::RFC3164));
    BOOST_TEST(*m.hostname == "192.168.1.1");
    BOOST_TEST(m.app_name.has_value());
    BOOST_TEST(m.message   == "Configured from console");
}

BOOST_AUTO_TEST_SUITE_END()
