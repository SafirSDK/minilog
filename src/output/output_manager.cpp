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

#include "output_manager.hpp"

namespace minilog
{

OutputManager::OutputManager(boost::asio::io_context& ioc, const Config& cfg)
{
    for (const auto& outCfg : cfg.outputs)
    {
        m_sinks.push_back(std::make_unique<LogFile>(ioc, outCfg));
    }
}

void OutputManager::dispatch(const SyslogMessage& msg)
{
    for (auto& sink : m_sinks)
    {
        // TODO: facility matching, includeMalformed check
        sink->write(msg);
    }
}

void OutputManager::close()
{
    for (auto& sink : m_sinks)
    {
        sink->close();
    }
}

} // namespace minilog
