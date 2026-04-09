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
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
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

    if (cfg.forwarding.enabled && cfg.forwarding.host.empty())
    {
        throw std::runtime_error("[forwarding] enabled = true requires a host address");
    }

    return cfg;
}

} // namespace minilog
