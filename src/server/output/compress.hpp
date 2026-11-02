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

#include <atomic>
#include <filesystem>
#include <string>

namespace minilog
{

// The name a rotated generation has once compressed: "syslog.2.log" →
// "syslog.2.log.gz".
std::filesystem::path gzipPath(const std::filesystem::path& plain);

// The name it is written under until it is complete.
std::filesystem::path gzipTempPath(const std::filesystem::path& plain);

// Compress `plain` into gzipPath(plain), then remove `plain`.
//
// The gzip is written to gzipTempPath(plain), synced to disk and only then
// renamed into place, and the plain file is removed last — so whatever stops
// this partway (a full disk, a crash, power loss) leaves the plain file intact,
// at worst beside a .tmp that the next startup removes. Checks `cancel`
// between blocks and gives up, removing the .tmp, once it is set.
//
// Returns an empty string on success, or what went wrong. Never throws.
std::string gzipFile(const std::filesystem::path& plain, const std::atomic<bool>& cancel);

} // namespace minilog
