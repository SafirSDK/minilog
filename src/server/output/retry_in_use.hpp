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

#include <chrono>
#include <system_error>
#include <thread>

namespace minilog
{

// Run a filesystem operation that reports through `ec`, retrying it briefly if
// the file is in use. On Windows a file that another program has open without
// sharing delete cannot be renamed or removed; antivirus does exactly that to a
// file it has just seen closed, typically for milliseconds. Four retries over
// 750 ms let it go first. Rotation uses this so that a scan does not count as a
// failed rotation, and compression so that a scan of the .gz it has just
// written does not leave the generation uncompressed for good. Elsewhere an
// open file never blocks either operation, and nothing is retried.
template <typename Op> void retryWhileInUse(Op op, std::error_code& ec)
{
    op(ec);
#ifdef _WIN32
    constexpr int kAccessDenied     = 5;  // ERROR_ACCESS_DENIED: also a pending delete
    constexpr int kSharingViolation = 32; // ERROR_SHARING_VIOLATION
    for (const int ms : {50, 100, 200, 400})
    {
        if (!ec || (ec.value() != kSharingViolation && ec.value() != kAccessDenied))
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        ec.clear();
        op(ec);
    }
#endif
}

} // namespace minilog
