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

#include "syslog_format.hpp"

#include <chrono>
#include <stdexcept>

namespace minilog::send
{

namespace
{

constexpr const char* kMonths[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string pri(const SyslogFields& f)
{
    return "<" + std::to_string(f.facility * 8 + f.severity) + ">";
}

std::string nilable(const std::optional<std::string>& v)
{
    return v ? *v : "-";
}

// A non-negative number, left-padded to `width` with `fill`.
std::string padded(int value, std::size_t width, char fill)
{
    std::string s = std::to_string(value);
    return s.size() >= width ? s : std::string(width - s.size(), fill) + s;
}

// RFC 5424 §6: header fields are PRINTUSASCII, which is %d33-126 — anything
// printable but a space. This is also what keeps a value from being split into
// two fields, or from ending the header early, at the receiver.
bool isPrintableAsciiToken(std::string_view s)
{
    if (s.empty())
    {
        return false;
    }
    for (const unsigned char c : s)
    {
        if (c < 33 || c > 126)
        {
            return false;
        }
    }
    return true;
}

void requireToken(const char* what, std::string_view value)
{
    if (!isPrintableAsciiToken(value))
    {
        throw std::runtime_error(std::string(what) + " must be one word of printable ASCII, got '" +
                                 std::string(value) + "'");
    }
}

void requireAtMost(const char* what, std::string_view value, std::size_t max, const char* rfc)
{
    if (value.size() > max)
    {
        throw std::runtime_error(std::string(what) + " is " + std::to_string(value.size()) +
                                 " characters; " + rfc + " allows at most " + std::to_string(max));
    }
}

void requireNoneOf(const char* what, std::string_view value, std::string_view forbidden)
{
    if (value.find_first_of(forbidden) != std::string_view::npos)
    {
        throw std::runtime_error(std::string(what) + " cannot contain any of '" +
                                 std::string(forbidden) + "' in an RFC 3164 message, got '" +
                                 std::string(value) + "'");
    }
}

} // namespace

std::string formatRfc5424(const SyslogFields& f)
{
    // "1" is the version; the lone "-" is empty structured data.
    return pri(f) + "1 " + f.timestamp + " " + f.hostname + " " + f.app + " " + nilable(f.pid) +
           " " + nilable(f.msgid) + " - " + f.message;
}

std::string formatRfc3164(const SyslogFields& f)
{
    std::string tag = f.app;
    if (f.pid)
    {
        tag += "[" + *f.pid + "]";
    }
    return pri(f) + f.timestamp + " " + f.hostname + " " + tag + ": " + f.message;
}

void validateFields(const SyslogFields& f, bool rfc3164)
{
    if (f.facility < 0 || f.facility > 23)
    {
        throw std::runtime_error("facility must be 0-23, got " + std::to_string(f.facility));
    }
    if (f.severity < 0 || f.severity > 7)
    {
        throw std::runtime_error("severity must be 0-7, got " + std::to_string(f.severity));
    }
    requireToken("hostname", f.hostname);
    requireToken("app", f.app);
    if (f.pid)
    {
        requireToken("pid", *f.pid);
    }
    if (rfc3164)
    {
        // The tag is APP[PID]: — the receiver finds the pid by the brackets and
        // the end of the tag by the colon, so those characters would move text
        // between fields.
        requireNoneOf("app", f.app, "[]:");
        if (f.pid)
        {
            requireNoneOf("pid", *f.pid, "[]:");
        }
        if (f.msgid)
        {
            throw std::runtime_error("--msgid has no field to go in with --rfc3164");
        }
        // RFC 3164 §4.1.3: the TAG must not exceed 32 characters. It also says
        // the TAG is alphanumeric, which no real-world tag (`minilog-send`,
        // `systemd-logind`) honours, so only the length is enforced.
        const std::string tag = f.pid ? f.app + "[" + *f.pid + "]" : f.app;
        requireAtMost("the tag APP[PID]", tag, 32, "RFC 3164");
    }
    else
    {
        // RFC 5424 §6.2: the header fields have fixed maximum lengths. minilog
        // does not care, but another collector may drop what exceeds them.
        requireAtMost("hostname", f.hostname, 255, "RFC 5424");
        requireAtMost("app", f.app, 48, "RFC 5424");
        if (f.pid)
        {
            requireAtMost("pid", *f.pid, 128, "RFC 5424");
        }
        if (f.msgid)
        {
            requireToken("msgid", *f.msgid);
            requireAtMost("msgid", *f.msgid, 32, "RFC 5424");
        }
    }
    if (f.message.empty())
    {
        throw std::runtime_error("the message is empty");
    }
}

LocalTime localNow()
{
    using namespace std::chrono;
    const auto now      = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const auto micros   = duration_cast<microseconds>(now - system_clock::from_time_t(t)).count();

    LocalTime lt;
#ifdef _WIN32
    localtime_s(&lt.tm, &t);
    // No tm_gmtoff on MSVC: the zone's standard offset plus the DST adjustment
    // in force for this particular time, both as "seconds west", hence the sign.
    long tz = 0, dst = 0;
    _get_timezone(&tz);
    _get_dstbias(&dst);
    lt.utcOffsetSeconds = -static_cast<int>(tz + (lt.tm.tm_isdst > 0 ? dst : 0));
#else
    localtime_r(&t, &lt.tm);
    lt.utcOffsetSeconds = static_cast<int>(lt.tm.tm_gmtoff);
#endif
    // A negative remainder happens for times before the epoch only; clamp so
    // the field cannot come out as "-000001".
    lt.microseconds = micros < 0 ? 0 : static_cast<int>(micros);
    return lt;
}

std::string rfc5424Timestamp(const LocalTime& t)
{
    const int offset = t.utcOffsetSeconds;
    const char sign  = offset < 0 ? '-' : '+';
    const int absOff = offset < 0 ? -offset : offset;
    return padded(t.tm.tm_year + 1900, 4, '0') + "-" + padded(t.tm.tm_mon + 1, 2, '0') + "-" +
           padded(t.tm.tm_mday, 2, '0') + "T" + padded(t.tm.tm_hour, 2, '0') + ":" +
           padded(t.tm.tm_min, 2, '0') + ":" + padded(t.tm.tm_sec, 2, '0') + "." +
           padded(t.microseconds, 6, '0') + sign + padded(absOff / 3600, 2, '0') + ":" +
           padded((absOff % 3600) / 60, 2, '0');
}

std::string rfc3164Timestamp(const LocalTime& t)
{
    return std::string(kMonths[t.tm.tm_mon]) + " " + padded(t.tm.tm_mday, 2, ' ') + " " +
           padded(t.tm.tm_hour, 2, '0') + ":" + padded(t.tm.tm_min, 2, '0') + ":" +
           padded(t.tm.tm_sec, 2, '0');
}

std::vector<std::string> splitLines(std::string_view text)
{
    std::vector<std::string> lines;
    while (!text.empty())
    {
        const auto nl         = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (!line.empty())
        {
            lines.emplace_back(line);
        }
    }
    return lines;
}

} // namespace minilog::send
