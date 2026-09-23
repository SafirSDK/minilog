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

#pragma once
#include <cstddef>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minilog::send
{

// Everything that goes into one datagram. Pure data: main() fills it in from
// the command line and the clock, and the functions below turn it into bytes,
// so the formatting is testable without a socket or a real time of day.
struct SyslogFields
{
    int facility = 1; // user
    int severity = 6; // info
    std::string timestamp;
    std::string hostname;
    std::string app;
    std::optional<std::string> pid;
    std::optional<std::string> msgid; // RFC 5424 only
    std::string message;
};

// The largest UDP payload there is (65535 − 20 IPv4 − 8 UDP). A datagram over
// this cannot be sent at all, so it is refused rather than truncated: the
// forwarder truncates because it is unattended, and this tool is not.
constexpr std::size_t kMaxDatagramBytes = 65507;

// <PRI>1 TIMESTAMP HOSTNAME APP PID MSGID - MSG
std::string formatRfc5424(const SyslogFields& f);

// <PRI>Mmm dd hh:mm:ss HOSTNAME APP[PID]: MSG
std::string formatRfc3164(const SyslogFields& f);

// Check the fields against the format's grammar and throw std::runtime_error
// naming the first problem. The parser on the other end takes the header
// apart on spaces (and, for RFC 3164, on the tag's brackets and colon), so a
// value containing one of those would come back as a different field. The
// RFCs' length limits (RFC 5424: HOSTNAME 255, APP-NAME 48, PROCID 128,
// MSGID 32; RFC 3164: TAG 32) are enforced too, for the sake of collectors
// stricter than minilog.
void validateFields(const SyslogFields& f, bool rfc3164);

// A local wall-clock time, broken down and with its offset from UTC, so that a
// timestamp can be formatted from a fixed value in a test.
struct LocalTime
{
    std::tm tm{};
    int microseconds     = 0;
    int utcOffsetSeconds = 0;
};

// The time now, in the local zone.
LocalTime localNow();

// 2026-09-23T14:07:31.123456+02:00
std::string rfc5424Timestamp(const LocalTime& t);

// Sep 23 14:07:31 — day space-padded, as RFC 3164 §4.1.2 wants it.
std::string rfc3164Timestamp(const LocalTime& t);

// Split stdin into messages: one per line, LF or CRLF, trailing newline
// optional. Empty lines are dropped — a blank line is not a message, and one
// at the end of a command's output is usual.
std::vector<std::string> splitLines(std::string_view text);

} // namespace minilog::send
