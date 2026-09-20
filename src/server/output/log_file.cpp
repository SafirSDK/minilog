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

#include "log_file.hpp"

#include "platform/os_log.hpp"

#include <boost/asio/post.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <system_error>

namespace minilog
{

namespace
{

// Build the rotated filename for generation n.
// E.g. "syslog.log" → "syslog.1.log", "syslog.2.log", …
std::filesystem::path rotatedPath(const std::filesystem::path& base, int n)
{
    return base.parent_path() /
           std::format("{}.{}{}", base.stem().string(), n, base.extension().string());
}

std::string currentTimestamp()
{
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

} // namespace

// Replace every invalid UTF-8 byte sequence with U+FFFD (0xEF 0xBF 0xBD).
// This ensures all string fields written to JSONL are valid UTF-8 regardless
// of the encoding of the incoming syslog datagram.
std::string sanitizeUtf8(std::string_view s)
{
    constexpr std::string_view kReplacement = "\xef\xbf\xbd";
    std::string out;
    out.reserve(s.size());

    for (std::size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i]);

        if (c <= 0x7F)
        {
            out += s[i++];
            continue;
        }

        // Determine sequence length and first-continuation-byte bounds.
        std::size_t len;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF)
        {
            len = 2;
        }
        else if (c == 0xE0)
        {
            len = 3;
            lo  = 0xA0;
        }
        else if (c >= 0xE1 && c <= 0xEC)
        {
            len = 3;
        }
        else if (c == 0xED)
        {
            len = 3;
            hi  = 0x9F;
        }
        else if (c >= 0xEE && c <= 0xEF)
        {
            len = 3;
        }
        else if (c == 0xF0)
        {
            len = 4;
            lo  = 0x90;
        }
        else if (c >= 0xF1 && c <= 0xF3)
        {
            len = 4;
        }
        else if (c == 0xF4)
        {
            len = 4;
            hi  = 0x8F;
        }
        else
        {
            out += kReplacement; // invalid lead byte
            ++i;
            continue;
        }

        if (i + len > s.size())
        {
            out += kReplacement; // truncated sequence
            ++i;
            continue;
        }

        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if (b1 < lo || b1 > hi)
        {
            out += kReplacement; // first continuation byte out of range
            ++i;
            continue;
        }

        bool valid = true;
        for (std::size_t j = 2; j < len; ++j)
        {
            const auto bj = static_cast<unsigned char>(s[i + j]);
            if (bj < 0x80 || bj > 0xBF)
            {
                valid = false;
                break;
            }
        }

        if (valid)
        {
            out.append(s.data() + i, len);
            i += len;
        }
        else
        {
            out += kReplacement;
            ++i;
        }
    }

    return out;
}

// Escape control characters so that one datagram always occupies exactly one
// line in the text sink. Without this a sender can embed a newline and author a
// second entry that is indistinguishable from a genuine one — including its own
// PRI, so it appears to come from a facility the datagram never had. The same
// escaping keeps a terminal from acting on the file when it is paged or cat'd.
//
// Doubling the backslash is what makes the transform reversible, and the two
// escape forms say what they encode: \xNN is one byte, \uNNNN is one codepoint.
// Both are fixed-width, so unlike C a hex escape never swallows the text that
// follows it.
//
// This is not JSON's escaping and does not try to be — boost::json writes ESC
// as \u001B, TAB as \t and quotes the double quote, none of which this does.
// It is the dialect the cli-viewer displays, so a line on screen reads the way
// a line in the file does.
std::string escapeControlChars(std::string_view s)
{
    std::string out;
    out.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(s[i]);

        // C1 controls arrive as the two-byte sequence 0xC2 0x80-0xC2 0x9F, and
        // U+009B and U+009D are the 8-bit forms of CSI and OSC — the sequences
        // the C0 branch below exists to stop, reachable without an ESC byte at
        // all. Matching the decoded codepoint rather than the raw byte is what
        // keeps this from mangling other scripts: 0x80-0x9F is also the range
        // the continuation bytes of ordinary text fall in, 0xE6 0x97 0xA5 for
        // U+65E5 among them.
        if (c == 0xC2 && i + 1 < s.size())
        {
            const auto next = static_cast<unsigned char>(s[i + 1]);
            if (next >= 0x80 && next <= 0x9F)
            {
                out += std::format("\\u{:04X}", next);
                ++i;
                continue;
            }
        }

        if (c == '\\')
        {
            out += "\\\\";
        }
        else if (c == '\n')
        {
            out += "\\n";
        }
        else if (c == '\r')
        {
            out += "\\r";
        }
        else if ((c < 0x20 && c != '\t') || c == 0x7F)
        {
            out += std::format("\\x{:02X}", c);
        }
        else
        {
            out += s[i];
        }
    }

    return out;
}

namespace
{

std::string toJsonlRecord(const SyslogMessage& msg, const std::string& rcv)
{
    namespace bj = boost::json;

    auto optStr = [](const std::optional<std::string>& o) -> bj::value
    { return o ? bj::value(sanitizeUtf8(*o)) : bj::value(nullptr); };

    bj::object obj;
    obj["rcv"]      = rcv;
    obj["src"]      = msg.srcIp;
    obj["proto"]    = (msg.protocol == Protocol::RFC3164)   ? "RFC3164"
                      : (msg.protocol == Protocol::RFC5424) ? "RFC5424"
                                                            : "UNKNOWN";
    obj["facility"] = optStr(msg.facilityName);
    obj["severity"] = optStr(msg.severityName);
    obj["hostname"] = optStr(msg.hostname);
    obj["app"]      = optStr(msg.appName);
    obj["pid"]      = optStr(msg.procId);
    obj["msgid"]    = optStr(msg.msgId);
    obj["msg_time"] = optStr(msg.timestamp);
    obj["message"]  = sanitizeUtf8(msg.message);

    return bj::serialize(obj);
}

} // namespace

LogFile::LogFile(boost::asio::io_context& ioc, OutputConfig cfg)
    : m_cfg(std::move(cfg)), m_strand(boost::asio::make_strand(ioc))
{
}

LogFile::~LogFile()
{
    // Do not call closeFiles() here — it would race with strand work that may
    // still be queued.  The std::ofstream members close themselves on
    // destruction.  For an explicit close, use close() which posts to the
    // strand.
}

bool LogFile::openAtStartup()
{
    openFiles();
    return !m_closed;
}

void LogFile::write(const SyslogMessage& msg)
{
    boost::asio::post(m_strand,
                      [this, msg]()
                      {
                          // Last line of defence. Anything escaping here unwinds out of
                          // io_context::run() and terminates the process, taking every
                          // other sink and the forwarder with it.
                          try
                          {
                              doWrite(msg);
                          }
                          catch (const std::exception& e)
                          {
                              failSinkFromHandler(e.what());
                          }
                          catch (...)
                          {
                              failSinkFromHandler("non-standard exception");
                          }
                      });
}

void LogFile::close()
{
    boost::asio::post(m_strand,
                      [this]()
                      {
                          m_closed = true;
                          try
                          {
                              closeFiles();
                          }
                          catch (...)
                          {
                              // Already closing; nothing useful left to do, and this
                              // handler must not throw.
                          }
                      });
}

void LogFile::failSink(const std::string& reason)
{
    osLogError("minilog: " + reason + "; closing sink");
    m_closed = true;
    closeFiles();
}

void LogFile::failSinkFromHandler(const char* what) noexcept
{
    try
    {
        failSink(std::string("unexpected exception in sink handler: ") + what);
    }
    catch (...)
    {
        // Reporting failed as well — still out of memory, most likely. Take the
        // sink out of service silently rather than let anything escape.
        m_closed = true;
    }
}

void LogFile::doWrite(const SyslogMessage& msg)
{
    if (m_closed)
    {
        return;
    }

    if (msg.protocol == Protocol::Unknown && !m_cfg.includeMalformed)
    {
        return;
    }

    // Lazy open on first write.
    const bool needOpen = (!m_cfg.textFile.empty() && !m_textStream.is_open()) ||
                          (!m_cfg.jsonlFile.empty() && !m_jsonlStream.is_open());
    if (needOpen)
    {
        openFiles();
    }

    // Rotate before this write if the previous write pushed us over the limit.
    rotateIfNeeded();

    const std::string rcv = currentTimestamp();

    if (m_textStream.is_open())
    {
        const std::string line = escapeControlChars(msg.raw) + "\n";
        m_textStream.write(line.data(), static_cast<std::streamsize>(line.size()));
        m_textStream.flush();
        if (!m_textStream)
        {
            failSink("write to '" + m_cfg.textFile + "' failed");
            return;
        }
        m_textSize += line.size();
    }

    if (m_jsonlStream.is_open())
    {
        const std::string record = toJsonlRecord(msg, rcv) + "\n";
        m_jsonlStream.write(record.data(), static_cast<std::streamsize>(record.size()));
        m_jsonlStream.flush();
        if (!m_jsonlStream)
        {
            failSink("write to '" + m_cfg.jsonlFile + "' failed");
            return;
        }
        m_jsonlSize += record.size();
    }
}

void LogFile::rotateIfNeeded()
{
    if (m_cfg.maxSize == 0)
    {
        return;
    }
    if (m_textSize >= m_cfg.maxSize || m_jsonlSize >= m_cfg.maxSize)
    {
        rotate();
    }
}

void LogFile::rotate()
{
    namespace fs = std::filesystem;

    closeFiles();

    // Every step below uses the error_code overloads: a throw from here would
    // escape the strand handler and abort the process. Any failure closes the
    // sink and abandons the rotation rather than pressing on with a half-shifted
    // chain, so shiftFiles reports whether it is still safe to continue.
    auto shiftFiles = [&](const fs::path& base)
    {
        // exists() with an error_code leaves ec clear for a file that simply is
        // not there, so nullopt means a real failure — a denied or unreachable
        // directory — rather than absence.
        auto probe = [&](const fs::path& p) -> std::optional<bool>
        {
            std::error_code ec;
            const bool found = fs::exists(p, ec);
            if (ec)
            {
                failSink("rotation probe failed for '" + p.string() + "': " + ec.message());
                return std::nullopt;
            }
            return found;
        };

        // Find the highest rotated generation that exists.
        // Probe up to the limit (or a reasonable cap when unlimited) so that
        // gaps left by manually deleted files don't cause orphaned generations.
        const int probeLimit = (m_cfg.maxFiles > 0) ? m_cfg.maxFiles : 1000;
        int highest          = 0;
        for (int n = 1; n <= probeLimit; ++n)
        {
            const auto found = probe(rotatedPath(base, n));
            if (!found)
            {
                return false;
            }
            if (*found)
            {
                highest = n;
            }
        }

        // Delete generations beyond max_files.
        if (m_cfg.maxFiles > 0 && highest >= m_cfg.maxFiles)
        {
            for (int n = highest; n >= m_cfg.maxFiles; --n)
            {
                const auto p     = rotatedPath(base, n);
                const auto found = probe(p);
                if (!found)
                {
                    return false;
                }
                if (!*found)
                {
                    continue;
                }
                std::error_code ec;
                fs::remove(p, ec);
                if (ec)
                {
                    failSink("rotation remove failed for '" + p.string() + "': " + ec.message());
                    return false;
                }
            }
            highest = m_cfg.maxFiles - 1;
        }

        // Shift existing rotated files up by one.
        for (int n = highest; n >= 1; --n)
        {
            const auto from  = rotatedPath(base, n);
            const auto found = probe(from);
            if (!found)
            {
                return false;
            }
            if (!*found)
            {
                continue;
            }
            std::error_code ec;
            fs::rename(from, rotatedPath(base, n + 1), ec);
            if (ec)
            {
                failSink("rotation rename failed for '" + from.string() + "': " + ec.message());
                return false;
            }
        }

        // Rename the current file to .1.
        const auto found = probe(base);
        if (!found)
        {
            return false;
        }
        if (*found)
        {
            std::error_code ec;
            fs::rename(base, rotatedPath(base, 1), ec);
            if (ec)
            {
                failSink("rotation rename failed for '" + base.string() + "': " + ec.message());
                return false;
            }
        }
        return true;
    };

    if (!m_cfg.textFile.empty() && !shiftFiles(fs::path(m_cfg.textFile)))
    {
        return;
    }
    if (!m_cfg.jsonlFile.empty() && !shiftFiles(fs::path(m_cfg.jsonlFile)))
    {
        return;
    }

    openFiles();
}

void LogFile::openFiles()
{
    namespace fs = std::filesystem;

    std::error_code ec;

    if (!m_cfg.textFile.empty())
    {
        m_textStream.open(m_cfg.textFile, std::ios::app | std::ios::binary);
        if (!m_textStream.is_open())
        {
            failSink("failed to open '" + m_cfg.textFile + "'");
            return;
        }
        const auto size = fs::file_size(m_cfg.textFile, ec);
        if (ec)
        {
            failSink("cannot determine size of '" + m_cfg.textFile + "': " + ec.message());
            return;
        }
        m_textSize = static_cast<uint64_t>(size);
    }

    if (!m_cfg.jsonlFile.empty())
    {
        m_jsonlStream.open(m_cfg.jsonlFile, std::ios::app | std::ios::binary);
        if (!m_jsonlStream.is_open())
        {
            failSink("failed to open '" + m_cfg.jsonlFile + "'");
            return;
        }
        const auto size = fs::file_size(m_cfg.jsonlFile, ec);
        if (ec)
        {
            failSink("cannot determine size of '" + m_cfg.jsonlFile + "': " + ec.message());
            return;
        }
        m_jsonlSize = static_cast<uint64_t>(size);
    }
}

void LogFile::closeFiles()
{
    if (m_textStream.is_open())
    {
        m_textStream.close();
    }
    if (m_jsonlStream.is_open())
    {
        m_jsonlStream.close();
    }
    m_textSize  = 0;
    m_jsonlSize = 0;
}

} // namespace minilog
