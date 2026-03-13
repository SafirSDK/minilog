#include "config.hpp"

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
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

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// Parse "auth,authpriv,*" → deduplicated vector<int>; empty vector = all (wildcard)
std::vector<int> parse_facilities(const std::string& raw)
{
    if (raw.empty())
    {
        return {};
    }

    std::vector<int> result;
    std::istringstream ss(raw);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        token = to_lower(trim(token));
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
uint64_t parse_size(const std::string& raw)
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

    const std::string unit = to_lower(trim(raw.substr(i)));
    uint64_t mult          = 1;
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

OutputConfig parse_output(const std::string& name, const boost::property_tree::ptree& sec)
{
    OutputConfig out;
    out.name       = name;
    out.text_file  = sec.get<std::string>("text_file", "");
    out.jsonl_file = sec.get<std::string>("jsonl_file", "");

    if (out.text_file.empty() && out.jsonl_file.empty())
    {
        throw std::runtime_error("[output." + name +
                                 "] must specify text_file, jsonl_file, or both");
    }

    const auto size_str = sec.get<std::string>("max_size", "");
    if (!size_str.empty())
    {
        out.max_size = parse_size(size_str);
    }

    out.max_files = sec.get<int>("max_files", out.max_files);
    if (out.max_files < 0)
    {
        throw std::runtime_error("[output." + name + "] max_files must be >= 0");
    }

    out.facilities        = parse_facilities(sec.get<std::string>("facility", "*"));
    out.include_malformed = sec.get<bool>("include_malformed", out.include_malformed);

    return out;
}

} // namespace

Config load_config(const std::string& path)
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
    cfg.host     = tree.get<std::string>("server.host", cfg.host);
    cfg.encoding = tree.get<std::string>("server.encoding", cfg.encoding);

    {
        const int port = tree.get<int>("server.udp_port", static_cast<int>(cfg.udp_port));
        if (port <= 0 || port > 65535)
        {
            throw std::runtime_error("Invalid udp_port: " + std::to_string(port));
        }
        cfg.udp_port = static_cast<uint16_t>(port);
    }
    {
        const int w = tree.get<int>("server.workers", cfg.workers);
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
            cfg.outputs.push_back(parse_output(name, sec));
        }
    }

    // [forwarding]
    if (auto fwd_node = tree.get_child_optional("forwarding"))
    {
        auto& f                = *fwd_node;
        cfg.forwarding.enabled = f.get<bool>("enabled", false);
        cfg.forwarding.host    = f.get<std::string>("host", "");
        cfg.forwarding.max_message_size =
            f.get<uint32_t>("max_message_size", cfg.forwarding.max_message_size);
        cfg.forwarding.facilities = parse_facilities(f.get<std::string>("facility", "*"));

        const int port = f.get<int>("port", static_cast<int>(cfg.forwarding.port));
        if (port <= 0 || port > 65535)
        {
            throw std::runtime_error("Invalid forwarding port: " + std::to_string(port));
        }
        cfg.forwarding.port = static_cast<uint16_t>(port);
    }

    return cfg;
}

} // namespace minilog
