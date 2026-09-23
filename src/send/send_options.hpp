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
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace minilog::send
{

// What the command line asked for. Defaults are the documented ones; the
// fields left empty here (hostname, pid) are filled in by main() from the
// machine, because a pure parser should not ask the OS anything.
struct SendOptions
{
    std::string host   = "127.0.0.1";
    std::uint16_t port = 514;
    int facility       = 1;              // user
    int severity       = 6;              // info
    std::string app    = "minilog-send";
    std::optional<std::string> hostname; // nullopt = this machine's
    std::optional<std::string> pid;      // nullopt = this process
    std::optional<std::string> msgid;
    bool rfc3164 = false;

    // The positional arguments, joined with single spaces. Empty means "read
    // stdin, one message per line".
    std::string message;

    bool help    = false;
    bool version = false;
};

// A command line that cannot be acted on. main() prints the message and the
// usage hint and exits 2, GNU-style, so that a script can tell "you asked for
// something impossible" from "the send failed".
struct UsageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Parse argv. Throws UsageError. Never touches the OS.
SendOptions parseSendOptions(int argc, const char* const argv[]);

// The text --help prints.
std::string usageText();

} // namespace minilog::send
