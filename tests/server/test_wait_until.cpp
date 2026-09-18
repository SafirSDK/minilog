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

// The timeout logic behind --stop and --uninstall. Those wait on the Windows
// SCM, which cannot be provoked into refusing to stop without a fault-injection
// hook, so the waiting itself is tested here instead — on every platform, and
// with a condition the test controls exactly.

#define BOOST_TEST_MODULE test_wait_until
#include "platform/wait_until.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>

using namespace minilog;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(wait_until_tests)

BOOST_AUTO_TEST_CASE(already_satisfied_returns_immediately)
{
    int calls = 0;

    const auto start = std::chrono::steady_clock::now();

    BOOST_CHECK(waitUntil(
        [&]
        {
            ++calls;
            return true;
        },
        10s,
        50ms));

    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_CHECK_LT(std::chrono::steady_clock::now() - start, 5s);
}

BOOST_AUTO_TEST_CASE(zero_timeout_still_checks_once)
{
    // Stopping a service that has already stopped must succeed even when the
    // caller allows no time at all for it.
    int calls = 0;

    BOOST_CHECK(waitUntil(
        [&]
        {
            ++calls;
            return true;
        },
        0s,
        50ms));
    BOOST_CHECK_EQUAL(calls, 1);

    calls = 0;
    BOOST_CHECK(!waitUntil(
        [&]
        {
            ++calls;
            return false;
        },
        0s,
        50ms));
    BOOST_CHECK_EQUAL(calls, 1);
}

BOOST_AUTO_TEST_CASE(succeeds_once_the_condition_becomes_true)
{
    int calls = 0;

    BOOST_CHECK(waitUntil(
        [&]
        {
            ++calls;
            return calls >= 3;
        },
        10s,
        1ms));

    BOOST_CHECK_EQUAL(calls, 3);
}

BOOST_AUTO_TEST_CASE(gives_up_after_the_timeout)
{
    int calls = 0;

    const auto start = std::chrono::steady_clock::now();

    BOOST_CHECK(!waitUntil(
        [&]
        {
            ++calls;
            return false;
        },
        200ms,
        20ms));

    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK_GE(elapsed, 200ms);
    // Polled repeatedly rather than sleeping the whole timeout away in one go.
    BOOST_CHECK_GT(calls, 1);
}

BOOST_AUTO_TEST_CASE(a_poll_interval_longer_than_the_timeout_does_not_overshoot)
{
    // The last sleep is clamped to the remaining time, so a coarse poll interval
    // cannot make the caller wait far past its deadline.
    const auto start = std::chrono::steady_clock::now();

    BOOST_CHECK(!waitUntil([] { return false; }, 100ms, 30s));

    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK_GE(elapsed, 100ms);
    BOOST_CHECK_LT(elapsed, 20s);
}

BOOST_AUTO_TEST_SUITE_END()
