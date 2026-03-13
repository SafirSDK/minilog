#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace minilog {

struct OutputConfig {
    std::string name;            // section name, e.g. "main"
    std::string text_file;       // empty = not configured
    std::string jsonl_file;      // empty = not configured
    uint64_t max_size = 0;       // bytes; 0 = unlimited
    int max_files     = 10;      // 0 = unlimited
    std::vector<int> facilities; // empty = all (wildcard)
    bool include_malformed = true;
};

struct ForwardingConfig {
    bool enabled = false;
    std::string host;
    uint16_t port             = 514;
    uint32_t max_message_size = 2048;
    std::vector<int> facilities; // empty = all
};

struct Config {
    std::string host     = "0.0.0.0";
    uint16_t udp_port    = 514;
    std::string encoding = "utf-8";
    int workers          = 4;

    std::vector<OutputConfig> outputs;
    ForwardingConfig forwarding;
};

// Load and validate config from an INI file.
// Throws std::runtime_error on parse or validation failure.
Config load_config(const std::string& path);

} // namespace minilog
