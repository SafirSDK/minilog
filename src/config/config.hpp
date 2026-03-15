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
#include <cstdint>
#include <string>
#include <vector>

namespace minilog
{

struct OutputConfig
{
    std::string name;            // section name, e.g. "main"
    std::string textFile;        // empty = not configured
    std::string jsonlFile;       // empty = not configured
    uint64_t maxSize = 0;        // bytes; 0 = unlimited
    int maxFiles     = 10;       // 0 = unlimited
    std::vector<int> facilities; // empty = all (wildcard)
    bool includeMalformed = true;
};

struct ForwardingConfig
{
    bool enabled = false;
    std::string host;
    uint16_t port           = 514;
    uint32_t maxMessageSize = 2048;
    std::vector<int> facilities; // empty = all
};

struct Config
{
    std::string host = "0.0.0.0";
    uint16_t udpPort = 514;
    int workers      = 4;

    std::vector<OutputConfig> outputs;
    ForwardingConfig forwarding;
};

// Load and validate config from an INI file.
// Throws std::runtime_error on parse or validation failure.
Config loadConfig(const std::string& path);

} // namespace minilog
