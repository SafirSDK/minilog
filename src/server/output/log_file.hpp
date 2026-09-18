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
#include <string>

namespace minilog
{

// Replace every invalid UTF-8 byte sequence with U+FFFD (0xEF 0xBF 0xBD).
std::string sanitizeUtf8(std::string_view s);

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

    // Report a failure and take this sink out of service: every later write is
    // dropped. Deliberately permanent — nothing reopens a closed sink, so a
    // storage problem degrades one sink instead of stopping the process or
    // producing one error per message for as long as the problem lasts.
    void failSink(const std::string& reason);

    // failSink for use from a handler's catch block. Never throws: a second
    // failure while reporting the first (out of memory, say) must not escape
    // into io_context::run(), which is where it would become std::terminate.
    void failSinkFromHandler(const char* what) noexcept;

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
