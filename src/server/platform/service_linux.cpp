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

#include "service.hpp"

#include <boost/asio/signal_set.hpp>

#include <csignal>
#include <memory>
#include <optional>

namespace minilog
{

void setupShutdown(boost::asio::io_context& ioc, std::function<void()> onStop)
{
    auto signals = std::make_shared<boost::asio::signal_set>(ioc, SIGTERM, SIGINT);
    signals->async_wait(
        [signals, onStop = std::move(onStop)](const boost::system::error_code& ec, int /*signum*/)
        {
            if (!ec)
            {
                onStop();
            }
        });
}

std::optional<int> tryRunAsService(const std::function<int()>& /*serviceMain*/)
{
    return std::nullopt;
}

void installService(const std::string& /*exePath*/, const std::string& /*configPath*/) {}

void uninstallService() {}

} // namespace minilog
