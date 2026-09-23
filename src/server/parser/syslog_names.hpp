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
#include <array>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

namespace minilog
{

// The names minilog gives a numeric facility and severity, and the names it
// accepts in their place. The number→name direction is what the parser writes
// into the JSONL `facility` and `severity` fields; the name→number direction is
// what a config file's `facility` key and minilog-send's command line take.
// They live in one header so that a name minilog-send accepts is a name the
// JSONL will show, and so that a config and a command line agree on aliases.
//
// Header-only on purpose: every test target compiles the parser or the config
// loader on its own, and a .cpp here would have to be added to each of them.

// Number → name, as written to the JSONL. Index is the numeric code.
//
// RFC 5424 gives both 9 and 15 to "clock daemon"; syslog(3) calls 9 LOG_CRON,
// so `cron` is an alias of 9 below, and 15 is `clock2`, the name the config has
// always accepted for it. Every name in this table must map back to its own
// index through facilityFromName — test_send checks that — which is what rules
// out calling 15 "cron", as an earlier version of this table did.
inline constexpr std::array<std::string_view, 24> kFacilityNames = {
    "kern",   "user",   "mail",     "daemon", "auth",   "syslog", "lpr",    "news",
    "uucp",   "clock",  "authpriv", "ftp",    "ntp",    "audit",  "alert",  "clock2",
    "local0", "local1", "local2",   "local3", "local4", "local5", "local6", "local7"};

inline constexpr std::array<std::string_view, 8> kSeverityNames = {
    "EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"};

namespace detail
{

// Name → number, including the aliases the README documents. Lower case; the
// lookups below fold their input before comparing.
inline constexpr std::array<std::pair<std::string_view, int>, 30> kFacilityAliases = {{
    {"kern", 0},      {"kernel", 0},  {"user", 1},     {"mail", 2},      {"daemon", 3},
    {"system", 3},    {"auth", 4},    {"security", 4}, {"syslog", 5},    {"lpr", 6},
    {"news", 7},      {"uucp", 8},    {"clock", 9},    {"cron", 9},      {"authpriv", 10},
    {"ftp", 11},      {"ntp", 12},    {"audit", 13},   {"logaudit", 13}, {"alert", 14},
    {"logalert", 14}, {"clock2", 15}, {"local0", 16},  {"local1", 17},   {"local2", 18},
    {"local3", 19},   {"local4", 20}, {"local5", 21},  {"local6", 22},   {"local7", 23},
}};

// The syslog(3) spellings as well as the long ones the JSONL uses, so that
// what somebody types from habit — `err`, `warn`, `crit` — is accepted.
inline constexpr std::array<std::pair<std::string_view, int>, 14> kSeverityAliases = {{
    {"emergency", 0},
    {"emerg", 0},
    {"panic", 0},
    {"alert", 1},
    {"critical", 2},
    {"crit", 2},
    {"error", 3},
    {"err", 3},
    {"warning", 4},
    {"warn", 4},
    {"notice", 5},
    {"info", 6},
    {"debug", 7},
    {"informational", 6},
}};

inline bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
std::optional<int> lookup(const std::array<std::pair<std::string_view, int>, N>& table,
                          std::string_view name)
{
    for (const auto& [alias, code] : table)
    {
        if (equalsIgnoreCase(alias, name))
        {
            return code;
        }
    }
    return std::nullopt;
}

} // namespace detail

// Name → number (0–23). Case-insensitive; aliases accepted. nullopt if unknown.
inline std::optional<int> facilityFromName(std::string_view name)
{
    return detail::lookup(detail::kFacilityAliases, name);
}

// Name → number (0–7). Case-insensitive; aliases accepted. nullopt if unknown.
inline std::optional<int> severityFromName(std::string_view name)
{
    return detail::lookup(detail::kSeverityAliases, name);
}

} // namespace minilog
