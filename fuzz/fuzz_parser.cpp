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

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    minilog::parseSyslog(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
