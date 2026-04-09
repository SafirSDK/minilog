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
        Sink sink;
        sink.facilities = outCfg.facilities;
        sink.file       = std::make_unique<LogFile>(ioc, outCfg);
        m_sinks.push_back(std::move(sink));
    }
}

void OutputManager::dispatch(const SyslogMessage& msg)
{
    for (auto& sink : m_sinks)
    {
        if (facilityMatches(sink.facilities, msg.facility))
        {
            sink.file->write(msg);
        }
    }
}

void OutputManager::close()
{
    for (auto& sink : m_sinks)
    {
        sink.file->close();
    }
}

} // namespace minilog
