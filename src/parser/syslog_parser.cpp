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

#include "syslog_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace minilog
{

namespace
{

const std::array<const char*, 8> kSeverityNames = {
    "EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"};

const std::array<const char*, 24> kFacilityNames = {
    "kern",   "user",   "mail",     "daemon", "auth",   "syslog", "lpr",    "news",
    "uucp",   "clock",  "authpriv", "ftp",    "ntp",    "audit",  "alert",  "cron",
    "local0", "local1", "local2",   "local3", "local4", "local5", "local6", "local7"};

void applyPri(SyslogMessage& msg, int pri)
{
    msg.pri      = pri;
    msg.facility = pri / 8;
    msg.severity = pri % 8;
    if (*msg.facility < static_cast<int>(kFacilityNames.size()))
    {
        msg.facilityName = kFacilityNames[static_cast<std::size_t>(*msg.facility)];
    }
    if (*msg.severity < static_cast<int>(kSeverityNames.size()))
    {
        msg.severityName = kSeverityNames[static_cast<std::size_t>(*msg.severity)];
    }
}

// Parse <NNN>. Returns PRI (0–191) and advances sv past '>'. Returns -1 on failure.
int parsePri(std::string_view& sv)
{
    if (sv.empty() || sv[0] != '<')
    {
        return -1;
    }
    sv.remove_prefix(1);
    int pri = 0, digits = 0;
    while (!sv.empty() && std::isdigit((unsigned char)sv[0]) && digits < 3)
    {
        pri = pri * 10 + (sv[0] - '0');
        sv.remove_prefix(1);
        ++digits;
    }
    if (digits == 0 || sv.empty() || sv[0] != '>')
    {
        return -1;
    }
    if (pri > 191)
    {
        return -1;
    }
    sv.remove_prefix(1);
    return pri;
}

// Parse next whitespace-delimited token. Returns nullopt for "-" or empty sv.
std::optional<std::string> parseNilable(std::string_view& sv)
{
    if (sv.empty())
    {
        return std::nullopt;
    }
    const auto end = sv.find(' ');
    std::string_view tok;
    if (end == std::string_view::npos)
    {
        tok = sv;
        sv  = {};
    }
    else
    {
        tok = sv.substr(0, end);
        sv.remove_prefix(end + 1);
    }
    if (tok == "-")
    {
        return std::nullopt;
    }
    return std::string(tok);
}

bool isMonthPrefix(std::string_view sv)
{
    static constexpr const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (sv.size() < 3)
    {
        return false;
    }
    for (const char* m : kMonths)
    {
        if (sv[0] == m[0] && sv[1] == m[1] && sv[2] == m[2])
        {
            return true;
        }
    }
    return false;
}

// Parse RFC3164 timestamp "Mon [D]D HH:MM:SS". Advances sv. Returns nullopt on mismatch.
std::optional<std::string> parse3164Timestamp(std::string_view& sv)
{
    if (!isMonthPrefix(sv))
    {
        return std::nullopt;
    }
    std::size_t p = 3;
    if (p >= sv.size() || sv[p] != ' ')
    {
        return std::nullopt;
    }
    while (p < sv.size() && sv[p] == ' ')
    {
        ++p;
    }
    const std::size_t day0 = p;
    while (p < sv.size() && std::isdigit((unsigned char)sv[p]))
    {
        ++p;
    }
    if (p - day0 == 0 || p - day0 > 2)
    {
        return std::nullopt;
    }
    if (p >= sv.size() || sv[p] != ' ')
    {
        return std::nullopt;
    }
    while (p < sv.size() && sv[p] == ' ')
    {
        ++p;
    }
    if (p + 8 > sv.size())
    {
        return std::nullopt;
    }
    const auto t = sv.substr(p, 8);
    if (!std::isdigit((unsigned char)t[0]) || !std::isdigit((unsigned char)t[1]) || t[2] != ':' ||
        !std::isdigit((unsigned char)t[3]) || !std::isdigit((unsigned char)t[4]) || t[5] != ':' ||
        !std::isdigit((unsigned char)t[6]) || !std::isdigit((unsigned char)t[7]))
    {
        return std::nullopt;
    }
    p += 8;
    std::string ts(sv.substr(0, p));
    sv.remove_prefix(p);
    if (!sv.empty() && sv[0] == ' ')
    {
        sv.remove_prefix(1);
    }
    return ts;
}

// Parse RFC3164 tag (e.g. "app[pid]:") and advance sv to message body.
std::pair<std::optional<std::string>, std::optional<std::string>> parse3164Tag(std::string_view& sv)
{
    const auto colon = sv.find(':');
    std::string_view tagPart;
    if (colon == std::string_view::npos)
    {
        // No colon: first word is tag, rest is message
        const auto sp = sv.find(' ');
        if (sp == std::string_view::npos)
        {
            tagPart = sv;
            sv      = {};
        }
        else
        {
            tagPart = sv.substr(0, sp);
            sv.remove_prefix(sp + 1);
        }
    }
    else
    {
        tagPart = sv.substr(0, colon);
        sv.remove_prefix(colon + 1);
        if (!sv.empty() && sv[0] == ' ')
        {
            sv.remove_prefix(1);
        }
    }

    // "app[pid]" or just "app"
    const auto bracket = tagPart.find('[');
    if (bracket != std::string_view::npos && !tagPart.empty() && tagPart.back() == ']')
    {
        auto app = std::string(tagPart.substr(0, bracket));
        auto pid = std::string(tagPart.substr(bracket + 1, tagPart.size() - bracket - 2));
        return {app.empty() ? std::nullopt : std::make_optional(std::move(app)),
                pid.empty() ? std::nullopt : std::make_optional(std::move(pid))};
    }
    while (!tagPart.empty() && tagPart.back() == ' ')
    {
        tagPart.remove_suffix(1);
    }
    return {tagPart.empty() ? std::nullopt : std::make_optional(std::string(tagPart)),
            std::nullopt};
}

bool tryRfc5424(std::string_view data, SyslogMessage& msg)
{
    std::string_view sv = data;
    const int pri       = parsePri(sv);
    if (pri < 0)
    {
        return false;
    }

    // Version: digit(s) followed by space; only version 1 is recognised
    std::size_t vi = 0;
    while (vi < sv.size() && std::isdigit((unsigned char)sv[vi]))
    {
        ++vi;
    }
    if (vi == 0 || vi >= sv.size() || sv[vi] != ' ')
    {
        return false;
    }
    int ver = 0;
    for (std::size_t j = 0; j < vi; ++j)
    {
        ver = ver * 10 + (sv[j] - '0');
    }
    if (ver != 1)
    {
        return false;
    }
    sv.remove_prefix(vi + 1);

    // Need at least 4 spaces for the 5 required header fields
    if (std::count(sv.begin(), sv.end(), ' ') < 4)
    {
        return false;
    }

    auto timestamp = parseNilable(sv);
    auto hostname  = parseNilable(sv);
    auto appName   = parseNilable(sv);
    auto procId    = parseNilable(sv);
    auto msgId     = parseNilable(sv);

    // Everything remaining (structured data + MSG) becomes the message.
    // Consistent with the Python reference: structured data is NOT stripped.
    msg.protocol = Protocol::RFC5424;
    msg.raw      = std::string(data);
    applyPri(msg, pri);
    msg.version   = ver;
    msg.timestamp = std::move(timestamp);
    msg.hostname  = std::move(hostname);
    msg.appName   = std::move(appName);
    msg.procId    = std::move(procId);
    msg.msgId     = std::move(msgId);
    msg.message   = std::string(sv);
    return true;
}

bool tryRfc3164(std::string_view data, SyslogMessage& msg)
{
    std::string_view sv = data;
    const int pri       = parsePri(sv);
    if (pri < 0)
    {
        return false;
    }

    auto ts = parse3164Timestamp(sv);
    if (!ts)
    {
        return false;
    }

    // Hostname: next whitespace-delimited word
    std::optional<std::string> hostname;
    {
        const auto sp = sv.find(' ');
        if (sp == std::string_view::npos)
        {
            if (!sv.empty())
            {
                hostname = std::string(sv);
            }
            sv = {};
        }
        else
        {
            hostname = std::string(sv.substr(0, sp));
            sv.remove_prefix(sp + 1);
        }
    }

    auto [appName, procId] = parse3164Tag(sv);

    msg.protocol = Protocol::RFC3164;
    msg.raw      = std::string(data);
    applyPri(msg, pri);
    msg.timestamp = std::move(ts);
    msg.hostname  = std::move(hostname);
    msg.appName   = std::move(appName);
    msg.procId    = std::move(procId);
    msg.message   = std::string(sv);
    return true;
}

} // namespace

SyslogMessage parseSyslog(std::string_view data)
{
    // Strip trailing CR/LF that some senders append
    while (!data.empty() && (data.back() == '\r' || data.back() == '\n'))
    {
        data.remove_suffix(1);
    }

    SyslogMessage msg;
    if (tryRfc5424(data, msg))
    {
        return msg;
    }
    if (tryRfc3164(data, msg))
    {
        return msg;
    }

    msg.protocol = Protocol::Unknown;
    msg.raw      = std::string(data);
    msg.message  = std::string(data);
    return msg;
}

} // namespace minilog
