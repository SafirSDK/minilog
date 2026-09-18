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

#include "platform/os_log.hpp"

#include <boost/asio/io_context.hpp>

#include <exception>
#include <string>

namespace minilog
{

// Run ioc until it runs out of work, surviving an exception thrown by a handler.
//
// Handlers are expected to catch their own failures — LogFile closes the sink
// that failed — so reaching the catch below means something was missed. It still
// must not be fatal: an exception that escapes io_context::run() on a
// std::thread's entry function cannot be caught anywhere else, and terminates
// the process along with every healthy sink and the forwarder.
//
// run() is re-entered afterwards. The handler that threw is gone, but the
// remaining work is not, and abandoning it would stop ingestion entirely — the
// outcome this exists to prevent.
inline void runIoContext(boost::asio::io_context& ioc)
{
    for (;;)
    {
        try
        {
            ioc.run();
            return;
        }
        catch (const std::exception& e)
        {
            osLogError(std::string("minilog: unhandled exception in io_context handler: ") +
                       e.what());
        }
        catch (...)
        {
            osLogError("minilog: unhandled non-standard exception in io_context handler");
        }
    }
}

} // namespace minilog
