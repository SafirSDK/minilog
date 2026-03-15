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

// libFuzzer entry point for the syslog parser.
// Build with the linux-fuzz CMake preset (Clang + -fsanitize=fuzzer,address).

#include "parser/syslog_parser.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const auto msg =
        minilog::parseSyslog(std::string_view(reinterpret_cast<const char*>(data), size));

    // Facility and severity are always derived from the same PRI, so they must
    // be set together or not at all.
    assert(msg.facility.has_value() == msg.severity.has_value());

    // raw is always populated unless the input is entirely CR/LF (which the
    // parser strips before processing, leaving nothing to record).
    const bool hasContent =
        std::any_of(data, data + size, [](uint8_t b) { return b != '\r' && b != '\n'; });
    assert(!hasContent || !msg.raw.empty());

    return 0;
}
