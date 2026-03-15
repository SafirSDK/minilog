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
    closeFiles();
}

void LogFile::write(const SyslogMessage& msg)
{
    boost::asio::post(m_strand, [this, msg]() { doWrite(msg); });
}

void LogFile::close()
{
    boost::asio::post(m_strand,
                      [this]()
                      {
                          m_closed = true;
                          closeFiles();
                      });
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
        const std::string line = msg.raw + "\n";
        m_textStream.write(line.data(), static_cast<std::streamsize>(line.size()));
        m_textStream.flush();
        if (!m_textStream)
        {
            osLogError("minilog: write to '" + m_cfg.textFile + "' failed; closing sink");
            m_closed = true;
            closeFiles();
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
            osLogError("minilog: write to '" + m_cfg.jsonlFile + "' failed; closing sink");
            m_closed = true;
            closeFiles();
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

    auto shiftFiles = [&](const fs::path& base)
    {
        if (base.empty())
        {
            return;
        }

        // Find how many rotated generations already exist.
        int highest = 0;
        for (int n = 1;; ++n)
        {
            if (fs::exists(rotatedPath(base, n)))
            {
                highest = n;
            }
            else
            {
                break;
            }
        }

        // Delete generations beyond max_files.
        if (m_cfg.maxFiles > 0 && highest >= m_cfg.maxFiles)
        {
            for (int n = highest; n >= m_cfg.maxFiles; --n)
            {
                auto p = rotatedPath(base, n);
                if (fs::exists(p))
                {
                    std::error_code ec;
                    fs::remove(p, ec);
                    if (ec)
                    {
                        osLogError("minilog: rotation remove failed for '" + p.string() +
                                   "': " + ec.message());
                    }
                }
            }
            highest = m_cfg.maxFiles - 1;
        }

        // Shift existing rotated files up by one.
        for (int n = highest; n >= 1; --n)
        {
            auto from = rotatedPath(base, n);
            if (fs::exists(from))
            {
                std::error_code ec;
                fs::rename(from, rotatedPath(base, n + 1), ec);
                if (ec)
                {
                    osLogError("minilog: rotation rename failed for '" + from.string() +
                               "': " + ec.message());
                }
            }
        }

        // Rename the current file to .1.
        if (fs::exists(base))
        {
            std::error_code ec;
            fs::rename(base, rotatedPath(base, 1), ec);
            if (ec)
            {
                osLogError("minilog: rotation rename failed for '" + base.string() +
                           "': " + ec.message());
            }
        }
    };

    if (!m_cfg.textFile.empty())
    {
        shiftFiles(fs::path(m_cfg.textFile));
    }
    if (!m_cfg.jsonlFile.empty())
    {
        shiftFiles(fs::path(m_cfg.jsonlFile));
    }

    openFiles();
}

void LogFile::openFiles()
{
    namespace fs = std::filesystem;

    if (!m_cfg.textFile.empty())
    {
        m_textStream.open(m_cfg.textFile, std::ios::app | std::ios::binary);
        if (!m_textStream.is_open())
        {
            osLogError("minilog: failed to open '" + m_cfg.textFile + "'; closing sink");
            m_closed = true;
            return;
        }
        m_textSize = static_cast<uint64_t>(fs::file_size(m_cfg.textFile));
    }

    if (!m_cfg.jsonlFile.empty())
    {
        m_jsonlStream.open(m_cfg.jsonlFile, std::ios::app | std::ios::binary);
        if (!m_jsonlStream.is_open())
        {
            osLogError("minilog: failed to open '" + m_cfg.jsonlFile + "'; closing sink");
            m_closed = true;
            closeFiles();
            return;
        }
        m_jsonlSize = static_cast<uint64_t>(fs::file_size(m_cfg.jsonlFile));
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
