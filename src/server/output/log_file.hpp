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
#include "sink_recovery.hpp"

#include "config/config.hpp"
#include "parser/syslog_message.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace minilog
{

// Replace every invalid UTF-8 byte sequence with U+FFFD (0xEF 0xBF 0xBD).
std::string sanitizeUtf8(std::string_view s);

// Escape control characters: C0 (0x00-0x1F), DEL (0x7F) and C1 (U+0080-U+009F,
// which carries the 8-bit CSI and OSC). Written as \n, \r, \xNN for a byte and
// \uNNNN for a codepoint, with a doubled backslash for a literal one, which is
// what keeps the transform reversible. TAB is left as-is: it cannot start a new
// line, and escaping it only hurts readability. Everything else above 0x7F
// passes through untouched, so ordinary UTF-8 text is never mangled.
std::string escapeControlChars(std::string_view s);

// Manages a pair of output files (text + jsonl) for one [output.X] section.
// All public methods are safe to call from multiple threads — writes are
// serialized through the strand.
class LogFile
{
public:
    // retryInterval is how long a sink closed by a filesystem error waits before
    // trying to open its files again. A parameter only so that tests can shorten
    // it — nothing outside the tests passes anything but the default.
    explicit LogFile(boost::asio::io_context& ioc,
                     OutputConfig cfg,
                     std::chrono::milliseconds retryInterval = SinkRecovery::kRetryInterval);
    ~LogFile();

    // Open the output files now, reporting failure instead of discovering it on
    // the first message. Returns false if the sink could not be opened.
    //
    // Startup only: this touches strand-owned state directly, which is safe just
    // while the calling thread is the only one running and nothing has been
    // posted to the strand yet.
    bool openAtStartup();

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
    // dropped, so a storage problem degrades one sink instead of stopping the
    // process or producing one error per message for as long as it lasts.
    //
    // Not permanent: scheduleRetry() arranges an attempt to open the files again,
    // and the reporting is rate-limited so that a fault which never clears cannot
    // fill the system log. See SinkRecovery for why the retry is on a timer.
    void failSink(const std::string& reason);

    // Arm the retry timer for a sink that has just been closed.
    void scheduleRetry();

    // Try to open the files again, reporting recovery if they open. A failure
    // here goes through failSink() like any other, which re-arms the timer.
    void attemptReopen();

    // failSink for use from a handler's catch block. Never throws: a second
    // failure while reporting the first (out of memory, say) must not escape
    // into io_context::run(), which is where it would become std::terminate.
    void failSinkFromHandler(const char* what) noexcept;

    OutputConfig m_cfg;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;
    const std::chrono::milliseconds m_retryInterval;

    // All fields below are accessed only on m_strand. The timer's handler runs
    // there too, since it is constructed with the strand as its executor.
    boost::asio::steady_timer m_retryTimer;
    SinkRecovery m_recovery;
    bool m_closed = false;
    // Set by close() and never cleared: a sink closed for shutdown must not be
    // reopened by a retry, and cancel() alone does not cover a handler that was
    // already queued when the close arrived.
    bool m_shuttingDown = false;
    std::ofstream m_textStream;
    std::ofstream m_jsonlStream;
    uint64_t m_textSize  = 0;
    uint64_t m_jsonlSize = 0;
};

} // namespace minilog
