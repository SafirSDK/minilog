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
#include "config/config.hpp"
#include "parser/syslog_message.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>

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

    // All fields below are accessed only on m_strand.
    bool m_closed = false;
    std::ofstream m_textStream;
    std::ofstream m_jsonlStream;
    uint64_t m_textSize  = 0;
    uint64_t m_jsonlSize = 0;
};

} // namespace minilog
