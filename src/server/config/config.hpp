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
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace minilog
{

// Upper bound on [output.*] max_files.
//
// Every generation is probed with a filesystem existence check — by rotate() on
// each rotation, and by the web viewer on each HTTP request as it builds its
// file chain — so an unbounded value is an unbounded amount of work in two
// places. 1000 is also the number the viewer already uses when max_files = 0
// means "keep all", so both ends agree on how deep a chain can ever be.
inline constexpr int kMaxFilesLimit = 1000;

struct OutputConfig
{
    std::string name;            // section name, e.g. "main"
    std::string textFile;        // empty = not configured
    std::string jsonlFile;       // empty = not configured
    uint64_t maxSize = 0;        // bytes; 0 = no rotation (the documented default)
    int maxFiles     = 10;       // 0 = keep every generation, up to kMaxFilesLimit
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

    // Bytes of accepted-but-not-yet-written datagrams held in memory before
    // further ones are dropped. See AdmissionControl in admission.hpp for why
    // this is measured in datagram bytes rather than queued bytes.
    uint64_t maxQueueBytes = 16ULL * 1024 * 1024;

    std::vector<OutputConfig> outputs;
    ForwardingConfig forwarding;
};

// Load and validate config from an INI file.
// Throws std::runtime_error on parse or validation failure.
Config loadConfig(const std::string& path);

// Returns true if msgFacility is accepted by the filter.
// An empty filter is a wildcard and accepts every message.
inline bool facilityMatches(const std::vector<int>& filter, const std::optional<int>& msgFacility)
{
    if (filter.empty())
    {
        return true; // wildcard — matches everything
    }
    if (!msgFacility)
    {
        return false; // message has no facility; only wildcard sinks receive it
    }
    return std::find(filter.begin(), filter.end(), *msgFacility) != filter.end();
}

} // namespace minilog
