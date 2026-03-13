#include "log_file.hpp"

namespace minilog
{

LogFile::LogFile(boost::asio::io_context& ioc, OutputConfig cfg)
    : m_cfg(std::move(cfg)), m_strand(boost::asio::make_strand(ioc))
{
}

LogFile::~LogFile()
{
    closeFiles();
}

void LogFile::write(const SyslogMessage& msg)
{
    boost::asio::post(m_strand, [this, msg]() { doWrite(msg); });
}

void LogFile::close()
{
    boost::asio::post(m_strand, [this]() { closeFiles(); });
}

void LogFile::doWrite(const SyslogMessage& /*msg*/)
{
    // TODO: implement rotation check, text write, jsonl write
}

void LogFile::rotateIfNeeded()
{
    // TODO: check file sizes, trigger rotate() if needed
}

void LogFile::rotate()
{
    // TODO: close, shift .N files, reopen
}

void LogFile::openFiles()
{
    // TODO: open textFile and jsonlFile for append
}

void LogFile::closeFiles()
{
    // TODO: flush and close open file handles
}

} // namespace minilog
