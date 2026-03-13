#include "output_manager.hpp"

namespace minilog
{

OutputManager::OutputManager(boost::asio::io_context& ioc, const Config& cfg)
{
    for (const auto& out_cfg : cfg.outputs)
    {
        m_sinks.push_back(std::make_unique<LogFile>(ioc, out_cfg));
    }
}

void OutputManager::dispatch(const SyslogMessage& msg)
{
    for (auto& sink : m_sinks)
    {
        // TODO: facility matching, include_malformed check
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
