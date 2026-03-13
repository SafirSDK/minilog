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

#include <boost/asio/post.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <ctime>
#include <string>

namespace minilog
{

namespace
{

// Build the rotated filename for generation n.
// E.g. "syslog.log" → "syslog.1.log", "syslog.2.log", …
std::filesystem::path rotatedPath(const std::filesystem::path& base, int n)
{
    auto parent = base.parent_path();
    auto stem   = base.stem().string();
    auto ext    = base.extension().string();
    return parent / (stem + "." + std::to_string(n) + ext);
}

std::string currentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string toJsonlRecord(const SyslogMessage& msg, const std::string& rcv)
{
    namespace bj = boost::json;

    auto optStr = [](const std::optional<std::string>& o) -> bj::value
    { return o ? bj::value(*o) : bj::value(nullptr); };

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
    obj["message"]  = msg.message;

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
        m_textSize += line.size();
    }

    if (m_jsonlStream.is_open())
    {
        const std::string record = toJsonlRecord(msg, rcv) + "\n";
        m_jsonlStream.write(record.data(), static_cast<std::streamsize>(record.size()));
        m_jsonlStream.flush();
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
                    fs::remove(p);
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
                fs::rename(from, rotatedPath(base, n + 1));
            }
        }

        // Rename the current file to .1.
        if (fs::exists(base))
        {
            fs::rename(base, rotatedPath(base, 1));
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
        if (m_textStream.is_open())
        {
            m_textSize = static_cast<uint64_t>(fs::file_size(m_cfg.textFile));
        }
    }

    if (!m_cfg.jsonlFile.empty())
    {
        m_jsonlStream.open(m_cfg.jsonlFile, std::ios::app | std::ios::binary);
        if (m_jsonlStream.is_open())
        {
            m_jsonlSize = static_cast<uint64_t>(fs::file_size(m_cfg.jsonlFile));
        }
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
