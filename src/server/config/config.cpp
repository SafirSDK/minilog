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

#include "config.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace minilog
{

namespace
{

// RFC5424 facility name → numeric code (0–23).
// Aliases (kern/kernel, auth/security, etc.) map to the same code.
const std::unordered_map<std::string, int> kFacilityNames = {
    {"kern", 0},      {"kernel", 0},  {"user", 1},     {"mail", 2},      {"daemon", 3},
    {"system", 3},    {"auth", 4},    {"security", 4}, {"syslog", 5},    {"lpr", 6},
    {"news", 7},      {"uucp", 8},    {"clock", 9},    {"cron", 9},      {"authpriv", 10},
    {"ftp", 11},      {"ntp", 12},    {"audit", 13},   {"logaudit", 13}, {"alert", 14},
    {"logalert", 14}, {"clock2", 15}, {"local0", 16},  {"local1", 17},   {"local2", 18},
    {"local3", 19},   {"local4", 20}, {"local5", 21},  {"local6", 22},   {"local7", 23},
};

// Reject an address that boost::asio cannot parse, naming the key and the value.
//
// Both hosts reach make_address() later — in UdpServer::start() and the Forwarder
// constructor — where the throwing overload would escape as a bare "Invalid
// argument" and, for the forwarding host, abort the process outright. Catching it
// here turns it into an ordinary config error that runServer reports via
// osLogError, which is what puts it in the Windows Event Log.
//
// Names are not resolved: make_address takes literals only. Both IPv4 and IPv6
// literals are accepted, since #24 settled that an IPv6 [server] host keeps
// working rather than being rejected.
//
// This is the rule for [server] host, which names an interface to bind. The
// forwarding destination is a different question and has its own check below.
void requireAddress(const std::string& label, const std::string& value)
{
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(value, ec);

    // Asio parses through WSAStringToAddressW on Windows — for both families —
    // which accepts a port suffix and quietly discards it. Both "10.0.0.5:514"
    // and the bracketed "[::1]:514" would therefore bind or forward to whatever
    // the port key says while ignoring what the user wrote, and only on Windows.
    // Screen both forms out textually so the answer is the same everywhere:
    // brackets are never valid input to make_address, and an IPv4 literal never
    // contains a colon.
    const bool bracketed =
        value.find('[') != std::string::npos || value.find(']') != std::string::npos;
    const bool strayPort = !ec && address.is_v4() && value.find(':') != std::string::npos;

    if (ec || bracketed || strayPort)
    {
        throw std::runtime_error("Invalid " + label + ": '" + value +
                                 "' is not an IP address (names are not resolved)");
    }
}

// A forwarding destination may be a hostname as well as an IP literal: naming
// the collector is the normal deployment shape, and hard-coding its address on
// every syslog host is what people are trying to avoid. The Forwarder resolves
// it, so validation here only has to rule out what cannot be a host at all.
//
// An unresolvable name is deliberately *not* a config error — a service that
// starts before DNS is up would otherwise fail its start — so anything rejected
// here is rejected on shape alone. The cases that matter:
//
//   - a port appended ("10.0.0.5:514", "[::1]:514"). Asio's Windows parser
//     accepts and silently discards the port, and as a name it would simply
//     never resolve, so the typo would be reported once and then retried
//     forever. Rejecting it here is the only place it is loud.
//   - brackets, which are a URL's IPv6 syntax rather than a host
//   - a colon in anything that is not a valid IPv6 literal, which is the same
//     port-suffix mistake in a different shape
//   - whitespace or a slash, which say the value is not a host name at all
//   - digits and dots that are not a valid address ("10.0.0.999"). A hostname
//     cannot have an all-numeric top-level label (RFC 1123 2.1), so this is a
//     mistyped address rather than a name, and it is worth saying so.
void requireDestinationHost(const std::string& label, const std::string& value)
{
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(value, ec);

    const bool bracketed =
        value.find('[') != std::string::npos || value.find(']') != std::string::npos;
    const bool looksLikeUrlOrPort =
        value.find(':') != std::string::npos || value.find('/') != std::string::npos;
    // "10.0.0.5:514" parses on Windows, where Asio goes through
    // WSAStringToAddressW, which accepts a port suffix and quietly discards it.
    // It has to be caught by its shape or it is a config error on Linux and a
    // silently wrong destination on the platform minilog is deployed to.
    const bool strayPort = !ec && address.is_v4() && value.find(':') != std::string::npos;
    const bool hasSpace  = std::any_of(
        value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });

    const bool digitsAndDots =
        std::all_of(value.begin(),
                    value.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0 || c == '.'; });

    // An IPv6 literal contains colons legitimately, so the colon and digits
    // rules only apply to values that did not parse as an address at all.
    if (bracketed || hasSpace || strayPort || (ec && (looksLikeUrlOrPort || digitsAndDots)))
    {
        throw std::runtime_error("Invalid " + label + ": '" + value +
                                 "' is neither an IP address nor a hostname (the port belongs in "
                                 "the 'port' key, not here)");
    }
}

// Reading a value that has to be translated, saying so when it cannot be.
//
// property_tree's get<T>(path, default) returns the default on a *translation
// failure* as well as on an absent key. So "max_files = abc" was silently 10 and
// "include_malformed = yess" silently true, while "max_sise = 100MB" — a typo one
// character away — was a startup failure naming the section and the key. A
// misspelled key and a misspelled value are the same operator mistake with the
// same consequence, a running server doing something other than what the file
// says, and only one of them was caught.
//
// label is the section and key as they appear in the file, e.g. "[output.main]
// max_files", so the operator is told where to look rather than which member of
// which struct failed to fill. Each of these returns defaultVal when the key is
// absent, which is still not an error.

int requireInt(const boost::property_tree::ptree& tree,
               const std::string& path,
               const std::string& label,
               int defaultVal)
{
    const auto raw = tree.get_optional<std::string>(path);
    if (!raw)
    {
        return defaultVal;
    }
    try
    {
        std::size_t pos;
        const int val = std::stoi(*raw, &pos);
        if (pos != raw->size())
        {
            throw std::invalid_argument("");
        }
        return val;
    }
    catch (...)
    {
        throw std::runtime_error(label + " = '" + *raw + "' is not a whole number");
    }
}

uint32_t requireUint32(const boost::property_tree::ptree& tree,
                       const std::string& path,
                       const std::string& label,
                       uint32_t defaultVal)
{
    const auto raw = tree.get_optional<std::string>(path);
    if (!raw)
    {
        return defaultVal;
    }
    // std::stoul accepts a leading '-' and wraps, so "-1" would come back as
    // 4294967295 rather than failing. Checked before parsing rather than after,
    // because by then the wrap has already happened.
    const bool negative = !raw->empty() && raw->front() == '-';
    try
    {
        std::size_t pos;
        const unsigned long val = std::stoul(*raw, &pos);
        if (negative || pos != raw->size() || val > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("");
        }
        return static_cast<uint32_t>(val);
    }
    catch (...)
    {
        throw std::runtime_error(label + " = '" + *raw + "' is not a whole number between 0 and " +
                                 std::to_string(std::numeric_limits<uint32_t>::max()));
    }
}

// Exactly the spellings property_tree's own translator accepted, and nothing
// else. Widening this to yes/no/on/off would be a larger promise than the
// mistake being fixed here needs, and one the documentation would then have to
// carry.
bool requireBool(const boost::property_tree::ptree& tree,
                 const std::string& path,
                 const std::string& label,
                 bool defaultVal)
{
    const auto raw = tree.get_optional<std::string>(path);
    if (!raw)
    {
        return defaultVal;
    }
    if (*raw == "true" || *raw == "1")
    {
        return true;
    }
    if (*raw == "false" || *raw == "0")
    {
        return false;
    }
    throw std::runtime_error(label + " = '" + *raw + "' is not 'true', 'false', '1' or '0'");
}

// Parse "auth,authpriv,*" → deduplicated vector<int>; empty vector = all (wildcard)
std::vector<int> parseFacilities(const std::string& raw)
{
    if (raw.empty())
    {
        return {};
    }

    std::vector<std::string> tokens;
    boost::algorithm::split(tokens, raw, boost::algorithm::is_any_of(","));

    std::vector<int> result;
    for (auto& token : tokens)
    {
        boost::algorithm::trim(token);
        boost::algorithm::to_lower(token);
        if (token == "*")
        {
            return {}; // wildcard → empty = all
        }
        auto it = kFacilityNames.find(token);
        if (it == kFacilityNames.end())
        {
            throw std::runtime_error("Unknown facility name: '" + token + "'");
        }
        const int code = it->second;
        if (std::find(result.begin(), result.end(), code) == result.end())
        {
            result.push_back(code);
        }
    }
    return result;
}

// Parse "100MB", "50KB", "2GB", "512B" → bytes.
//
// allowZero because max_size and max_queue_bytes disagree about what 0 means.
// For max_size it is "never rotate", which rotateIfNeeded implements and three
// separate places document. For max_queue_bytes there is deliberately no
// "unlimited" setting, so 0 stays an error there.
uint64_t parseSize(const std::string& raw, bool allowZero)
{
    if (raw.empty())
    {
        throw std::runtime_error("Empty size value");
    }

    std::size_t i = 0;
    while (i < raw.size() && std::isdigit((unsigned char)raw[i]))
    {
        ++i;
    }
    if (i == 0)
    {
        throw std::runtime_error("Invalid size (no numeric part): '" + raw + "'");
    }

    uint64_t num = 0;
    try
    {
        num = std::stoull(raw.substr(0, i));
    }
    catch (const std::out_of_range&)
    {
        // stoull throws std::out_of_range, which is a std::logic_error — so the
        // caller's catch for std::runtime_error missed it and the operator was
        // told "failed to load config: stoull", naming neither section nor key.
        throw std::runtime_error("Size out of range: '" + raw + "'");
    }
    if (num == 0 && !allowZero)
    {
        throw std::runtime_error("Size must be > 0: '" + raw + "'");
    }

    std::string unit = boost::algorithm::trim_copy(raw.substr(i));
    boost::algorithm::to_lower(unit);
    uint64_t mult = 1;
    if (unit == "b" || unit.empty())
    {
        mult = 1ULL;
    }
    else if (unit == "kb")
    {
        mult = 1024ULL;
    }
    else if (unit == "mb")
    {
        mult = 1024ULL * 1024;
    }
    else if (unit == "gb")
    {
        mult = 1024ULL * 1024 * 1024;
    }
    else
    {
        throw std::runtime_error("Unknown size unit '" + unit + "' in: '" + raw + "'");
    }

    // "17179869184GB" is 2^64 bytes, which wrapped to 0 — and 0 means "never
    // rotate", so a config asking for an enormous threshold silently became one
    // asking for none at all, and the sink grew until the disk filled.
    if (mult != 0 && num > std::numeric_limits<uint64_t>::max() / mult)
    {
        throw std::runtime_error("Size too large: '" + raw + "' overflows 64 bits");
    }

    return num * mult;
}

// A log file must be named by an absolute path. A relative one resolves against
// whatever the reading process happens to have as its working directory, and the
// three components that read this file have three different ones: the server's
// CWD, the cli-viewer's CWD, and (previously) the config file's directory in the
// web-viewer. One configuration therefore named up to three different files.
//
// The server's own case is the dangerous one: a service started by the SCM
// inherits C:\Windows\System32 as its CWD, so a relative path silently aimed at
// a system directory and left a dead sink behind when the open failed.
//
// std::filesystem decides what absolute means, which is what keeps UNC paths
// (\\server\share\logs) working on Windows. Note that a POSIX-rooted "/var/log/x"
// is *not* absolute on Windows — it has a root directory but no root name, making
// it relative to the current drive — so it is rejected there, correctly.
void requireAbsolutePath(const std::string& label, const std::string& value)
{
    if (!std::filesystem::path(value).is_absolute())
    {
        throw std::runtime_error(label + " = '" + value +
                                 "' must be an absolute path. Environment variables are not "
                                 "expanded, so a value such as '%ProgramData%\\minilog\\log' "
                                 "does not become one.");
    }
}

// Reject a key minilog does not understand, naming the section and the key.
//
// Every other config mistake in minilog fails loudly and says what it was;
// unrecognised keys were the last place one did not. "max_sise = 100MB" left the
// size at its default, "enabeld = true" left forwarding off and "faciltiy =
// auth" left the filter at the wildcard — each of them a working server doing
// something other than what the file says, with nothing to read anywhere.
//
// [web_viewer] belongs to the web viewer rather than to the server, but the
// server is the only component that validates this file, so a typo there would
// be just as silent. Its keys are therefore listed here too: a key added to the
// viewer has to be added here as well.
void requireKnownKeys(const std::string& section,
                      const boost::property_tree::ptree& sec,
                      const std::set<std::string>& known)
{
    // The loop finds the offending key; the message is built after it. Building
    // it in the loop body was the same work — the throw leaves on the first bad
    // key, so it ran at most once — but a concatenation chain inside a loop is
    // indistinguishable from a per-iteration one to a reader or an analyser.
    std::optional<std::string> unknown;
    for (const auto& [key, unused] : sec)
    {
        (void)unused;
        if (known.find(key) == known.end())
        {
            unknown = key;
            break;
        }
    }
    if (!unknown.has_value())
    {
        return;
    }

    std::string valid;
    for (const auto& k : known)
    {
        valid += (valid.empty() ? "" : ", ") + k;
    }
    throw std::runtime_error("Unknown key '" + *unknown + "' in [" + section +
                             "]. Valid keys are: " + valid);
}

OutputConfig parseOutput(const std::string& name, const boost::property_tree::ptree& sec)
{
    requireKnownKeys(
        "output." + name,
        sec,
        {"text_file", "jsonl_file", "max_size", "max_files", "facility", "include_malformed"});

    OutputConfig outCfg;
    outCfg.name      = name;
    outCfg.textFile  = sec.get<std::string>("text_file", "");
    outCfg.jsonlFile = sec.get<std::string>("jsonl_file", "");

    if (outCfg.textFile.empty() && outCfg.jsonlFile.empty())
    {
        throw std::runtime_error("[output." + name +
                                 "] must specify text_file, jsonl_file, or both");
    }
    if (!outCfg.textFile.empty())
    {
        requireAbsolutePath("[output." + name + "] text_file", outCfg.textFile);
    }
    if (!outCfg.jsonlFile.empty())
    {
        requireAbsolutePath("[output." + name + "] jsonl_file", outCfg.jsonlFile);
    }
    if (outCfg.textFile == outCfg.jsonlFile)
    {
        throw std::runtime_error("[output." + name +
                                 "] text_file and jsonl_file must name different files, "
                                 "but both are '" +
                                 outCfg.textFile + "'");
    }

    const auto sizeStr = sec.get<std::string>("max_size", "");
    if (!sizeStr.empty())
    {
        try
        {
            // 0 is accepted here: it is the documented way to disable rotation.
            outCfg.maxSize = parseSize(sizeStr, /*allowZero=*/true);
        }
        catch (const std::runtime_error& e)
        {
            throw std::runtime_error("[output." + name + "] " + e.what());
        }
    }

    // The upper bound is not cosmetic. rotate() probes every generation with a
    // filesystem existence check on each rotation, and the web viewer does the
    // same per HTTP request while building its file chain, so "max_files =
    // 2000000000" is two billion stat calls in both. The cap is the viewer's
    // existing sentinel for max_files = 0, so the two ends agree on how deep a
    // chain can ever be.
    outCfg.maxFiles =
        requireInt(sec, "max_files", "[output." + name + "] max_files", outCfg.maxFiles);
    if (outCfg.maxFiles < 0 || outCfg.maxFiles > kMaxFilesLimit)
    {
        throw std::runtime_error("[output." + name + "] max_files must be between 0 and " +
                                 std::to_string(kMaxFilesLimit) + " (0 = keep all, up to that)");
    }

    outCfg.facilities       = parseFacilities(sec.get<std::string>("facility", "*"));
    outCfg.includeMalformed = requireBool(sec,
                                          "include_malformed",
                                          "[output." + name + "] include_malformed",
                                          outCfg.includeMalformed);

    return outCfg;
}

// Every log file must belong to exactly one sink. Two writers on one path
// interleave their records, so whatever reads the file sees only the half it
// can parse; they shift the rotation generations once each per rotation, so
// half of max_files is consumed; and they size the file against a counter
// apiece, so it reaches roughly twice max_size before either one trips.
//
// Paths are compared as written. `.\a.log` and `a.log` slip through, which is
// accepted: the mistake worth catching is the same path typed twice.
void requireDistinctFiles(const std::vector<OutputConfig>& outputs)
{
    std::map<std::string, std::string> owners; // path -> "[output.x] field"

    const auto claim = [&owners](const std::string& file, const std::string& owner)
    {
        if (file.empty())
        {
            return;
        }
        const auto [it, inserted] = owners.emplace(file, owner);
        if (!inserted)
        {
            throw std::runtime_error(owner + " = '" + file + "' is already used by " + it->second +
                                     "; each log file must belong to exactly one output section");
        }
    };

    for (const auto& out : outputs)
    {
        claim(out.textFile, "[output." + out.name + "] text_file");
        claim(out.jsonlFile, "[output." + out.name + "] jsonl_file");
    }
}

} // namespace

Config loadConfig(const std::string& path)
{
    namespace pt = boost::property_tree;

    pt::ptree tree;
    try
    {
        pt::read_ini(path, tree);
    }
    catch (const pt::ini_parser_error& e)
    {
        throw std::runtime_error(std::string("Failed to parse config file: ") + e.what());
    }

    Config cfg;

    // [server]
    if (const auto serverNode = tree.get_child_optional("server"))
    {
        requireKnownKeys("server", *serverNode, {"host", "udp_port", "workers", "max_queue_bytes"});
    }
    cfg.host = tree.get<std::string>("server.host", cfg.host);
    requireAddress("[server] host", cfg.host);

    {
        const int port =
            requireInt(tree, "server.udp_port", "[server] udp_port", static_cast<int>(cfg.udpPort));
        if (port < 0 || port > 65535)
        {
            throw std::runtime_error("Invalid udp_port: " + std::to_string(port));
        }
        cfg.udpPort = static_cast<uint16_t>(port);
    }
    {
        // The upper bound matters as much as the lower one. runServer spawns a
        // std::thread per worker, so "workers = 1000000" — a misplaced digit —
        // used to loop until thread creation failed, and the std::system_error
        // escaped through runServer and main to std::terminate: a core dump
        // instead of a config error, and on Windows nothing in the Event Log
        // because osLogError was never reached.
        //
        // A flat cap rather than a multiple of hardware_concurrency: the number
        // has to mean the same thing on the machine that writes the config and
        // the one that runs it, and hardware_concurrency is also allowed to
        // return 0. 256 is far past anything useful for an I/O-bound server and
        // still nowhere near a thread limit.
        constexpr int kMaxWorkers = 256;
        const int w = requireInt(tree, "server.workers", "[server] workers", cfg.workers);
        if (w <= 0 || w > kMaxWorkers)
        {
            throw std::runtime_error("workers must be between 1 and " +
                                     std::to_string(kMaxWorkers) + " (got " + std::to_string(w) +
                                     ")");
        }
        cfg.workers = w;
    }
    {
        // Same syntax as max_size, so "64MB" and "67108864" both work.
        const auto raw = tree.get<std::string>("server.max_queue_bytes", "");
        if (!raw.empty())
        {
            try
            {
                cfg.maxQueueBytes = parseSize(raw, /*allowZero=*/false);
            }
            catch (const std::runtime_error& e)
            {
                throw std::runtime_error(std::string("[server] max_queue_bytes: ") + e.what());
            }
        }
    }

    // [output.X] — Boost PropertyTree's INI parser keeps the dot in section
    // names as a literal flat key ("output.main"), not a nested path.
    // Iterate the top-level tree and pick every key with the "output." prefix.
    {
        std::set<std::string> seen;
        for (auto& [key, sec] : tree)
        {
            constexpr std::string_view prefix = "output.";
            if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix)
            {
                continue;
            }
            const std::string name = key.substr(prefix.size());
            if (!seen.insert(name).second)
            {
                throw std::runtime_error("Duplicate output section name: '" + name + "'");
            }
            cfg.outputs.push_back(parseOutput(name, sec));
        }
    }

    if (cfg.outputs.empty())
    {
        throw std::runtime_error("Config must define at least one [output.*] section");
    }

    requireDistinctFiles(cfg.outputs);

    // [web_viewer] — read by the web viewer, not by minilog. Validated here
    // anyway, because this is the only component that validates the file at all.
    if (const auto viewerNode = tree.get_child_optional("web_viewer"))
    {
        requireKnownKeys("web_viewer", *viewerNode, {"host", "port"});
    }

    // [forwarding]
    if (auto fwdNode = tree.get_child_optional("forwarding"))
    {
        auto& f = *fwdNode;
        requireKnownKeys(
            "forwarding", f, {"enabled", "host", "port", "facility", "max_message_size"});
        cfg.forwarding.enabled        = requireBool(f, "enabled", "[forwarding] enabled", false);
        cfg.forwarding.host           = f.get<std::string>("host", "");
        cfg.forwarding.maxMessageSize = requireUint32(
            f, "max_message_size", "[forwarding] max_message_size", cfg.forwarding.maxMessageSize);
        cfg.forwarding.facilities = parseFacilities(f.get<std::string>("facility", "*"));

        const int port =
            requireInt(f, "port", "[forwarding] port", static_cast<int>(cfg.forwarding.port));
        if (port <= 0 || port > 65535)
        {
            throw std::runtime_error("Invalid forwarding port: " + std::to_string(port));
        }
        cfg.forwarding.port = static_cast<uint16_t>(port);
    }

    if (cfg.forwarding.enabled)
    {
        if (cfg.forwarding.host.empty())
        {
            throw std::runtime_error("[forwarding] enabled = true requires a host address");
        }
        // Only when enabled: a stale host under enabled = false harms nothing and
        // rejecting it would break configs that work today.
        requireDestinationHost("[forwarding] host", cfg.forwarding.host);
    }

    return cfg;
}

} // namespace minilog
