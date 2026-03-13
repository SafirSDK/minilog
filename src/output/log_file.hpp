#pragma once
#include "config/config.hpp"
#include "parser/syslog_message.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

namespace minilog
{

// Manages a pair of output files (text + jsonl) for one [output.X] section.
// All public methods are safe to call from multiple threads — writes are
// serialized through the strand.
class LogFile
{
public:
    explicit LogFile(boost::asio::io_context& ioc, OutputConfig cfg);
    ~LogFile();

    // Dispatch a write to this sink's strand (non-blocking for caller).
    void write(const SyslogMessage& msg);

    // Close files gracefully (called during shutdown).
    void close();

private:
    void doWrite(const SyslogMessage& msg);
    void rotateIfNeeded();
    void rotate();
    void openFiles();
    void closeFiles();

    OutputConfig m_cfg;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;

    // File handles and current sizes tracked here (accessed only on strand)
};

} // namespace minilog
