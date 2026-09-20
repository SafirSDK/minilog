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
#include <map>
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
    boost::asio::ip::make_address(value, ec);

    const bool bracketed =
        value.find('[') != std::string::npos || value.find(']') != std::string::npos;
    const bool looksLikeUrlOrPort =
        value.find(':') != std::string::npos || value.find('/') != std::string::npos;
    const bool hasSpace = std::any_of(
        value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });

    const bool digitsAndDots =
        std::all_of(value.begin(),
                    value.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0 || c == '.'; });

    // An IPv6 literal contains colons legitimately, so the colon and digits
    // rules only apply to values that did not parse as an address at all.
    if (bracketed || hasSpace || (ec && (looksLikeUrlOrPort || digitsAndDots)))
    {
        throw std::runtime_error("Invalid " + label + ": '" + value +
                                 "' is neither an IP address nor a hostname (the port belongs in "
                                 "the 'port' key, not here)");
    }
}

// Get an integer field from the property tree, throwing std::runtime_error for
// non-integer values (e.g. "abc"). Returns defaultVal when the key is absent.
int requireInt(const boost::property_tree::ptree& tree, const std::string& path, int defaultVal)
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
        throw std::runtime_error("Invalid integer value for '" + path + "': '" + *raw + "'");
    }
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

// Parse "100MB", "50KB", "2GB", "512B" → bytes
uint64_t parseSize(const std::string& raw)
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

    const uint64_t num = std::stoull(raw.substr(0, i));
    if (num == 0)
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

OutputConfig parseOutput(const std::string& name, const boost::property_tree::ptree& sec)
{
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
            outCfg.maxSize = parseSize(sizeStr);
        }
        catch (const std::runtime_error& e)
        {
            throw std::runtime_error("[output." + name + "] " + e.what());
        }
    }

    outCfg.maxFiles = sec.get<int>("max_files", outCfg.maxFiles);
    if (outCfg.maxFiles < 0)
    {
        throw std::runtime_error("[output." + name + "] max_files must be >= 0");
    }

    outCfg.facilities       = parseFacilities(sec.get<std::string>("facility", "*"));
    outCfg.includeMalformed = sec.get<bool>("include_malformed", outCfg.includeMalformed);

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
    cfg.host = tree.get<std::string>("server.host", cfg.host);
    requireAddress("[server] host", cfg.host);

    {
        const int port = requireInt(tree, "server.udp_port", static_cast<int>(cfg.udpPort));
        if (port < 0 || port > 65535)
        {
            throw std::runtime_error("Invalid udp_port: " + std::to_string(port));
        }
        cfg.udpPort = static_cast<uint16_t>(port);
    }
    {
        const int w = requireInt(tree, "server.workers", cfg.workers);
        if (w <= 0)
        {
            throw std::runtime_error("workers must be > 0");
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
                cfg.maxQueueBytes = parseSize(raw);
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

    // [forwarding]
    if (auto fwdNode = tree.get_child_optional("forwarding"))
    {
        auto& f                = *fwdNode;
        cfg.forwarding.enabled = f.get<bool>("enabled", false);
        cfg.forwarding.host    = f.get<std::string>("host", "");
        cfg.forwarding.maxMessageSize =
            f.get<uint32_t>("max_message_size", cfg.forwarding.maxMessageSize);
        cfg.forwarding.facilities = parseFacilities(f.get<std::string>("facility", "*"));

        const int port = requireInt(f, "port", static_cast<int>(cfg.forwarding.port));
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
