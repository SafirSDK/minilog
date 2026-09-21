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

#include "platform/service.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace minilog
{

// One thing --check found wrong, or worth saying.
struct Finding
{
    enum class Level
    {
        Error,  // minilog would not work as configured
        Warning // it would work, but not necessarily as intended
    };

    Level level = Level::Error;
    std::string message;
};

// What --check found. Deliberately a value: preflight() does the looking and
// nothing else, so every check is testable and the printing is separate.
struct PreflightReport
{
    // What this configuration needs from the machine, in plain terms: listen
    // endpoints and directories that need write access. Printed whether or not
    // anything is wrong, because the audience is often the person who has to
    // provision the ACLs and firewall rules rather than the one running --check.
    std::vector<std::string> requirements;

    std::vector<Finding> findings;

    [[nodiscard]] uint64_t count(Finding::Level level) const;

    // True if any finding is an Error, i.e. --check should exit non-zero.
    [[nodiscard]] bool failed() const { return count(Finding::Level::Error) != 0; }
};

// Validate a config file and the machine it is meant to run on, without
// starting anything and without creating or modifying a single file that
// outlives the call.
//
// Every check runs even after one has failed: a preflight that stops at the
// first problem forces a fix-rerun-fix-rerun cycle, which is the experience it
// exists to prevent. The one exception is a config that will not load at all,
// which leaves nothing to check the machine against.
//
// service - what the OS service manager says about minilog, from
//           queryServiceState(). It decides how a failed UDP bind is read: with
//           minilog already running, the port being taken is the expected state
//           of a healthy machine, not an error. Passed in rather than queried
//           here so the classification is testable on a host with no SCM.
//
// Never logs through osLogError: on Windows that writes to the Event Log, and a
// validation run must not leave entries behind — least of all before --install
// has registered the event source.
PreflightReport preflight(const std::string& configPath, ServiceState service);

// Write a report in human-readable form and return the process exit code.
// There is no machine-readable form: the consumer is a person diagnosing a host.
int printPreflight(const PreflightReport& report, const std::string& configPath, std::ostream& out);

} // namespace minilog
