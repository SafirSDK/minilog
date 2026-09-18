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
#include <chrono>
#include <functional>
#include <thread>

namespace minilog
{

// Poll isDone until it reports true or timeout elapses.
//
// isDone is always called at least once, so a zero timeout still gives the
// condition one chance: stopping a service that is already stopped must succeed
// however little patience the caller has.
//
// The last sleep is clamped to what is left of the timeout, so the call does not
// overshoot its deadline by up to a whole poll interval.
//
// Returns true if isDone reported true in time, false if the timeout ran out.
//
// Header-only, and free of anything Windows-specific, so that the timeout logic
// can be tested on Linux — the SCM code it serves cannot even be compiled there.
inline bool waitUntil(const std::function<bool()>& isDone,
                      std::chrono::steady_clock::duration timeout,
                      std::chrono::steady_clock::duration pollInterval)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (isDone())
        {
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::min(pollInterval, deadline - now));
    }
}

} // namespace minilog
