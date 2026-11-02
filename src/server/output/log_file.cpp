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

#include "compress.hpp"

#include "platform/os_log.hpp"

#include <boost/asio/post.hpp>
#include <boost/json.hpp>

#include <array>
#include <chrono>
#include <format>
#include <future>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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

// The retry interval, for the log line that announces it. Whole seconds in
// production; tests shorten the interval to milliseconds and the line should
// still read as a duration rather than as "0 s".
std::string formatInterval(std::chrono::milliseconds interval)
{
    if (interval % std::chrono::seconds{1} == std::chrono::milliseconds::zero())
    {
        return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(interval).count()) +
               " s";
    }
    return std::to_string(interval.count()) + " ms";
}

// Run a filesystem operation that reports through `ec`, retrying it briefly if
// the file is in use. On Windows a file that another program has open without
// sharing delete cannot be renamed or removed; antivirus does exactly that to a
// file it has just seen closed, typically for milliseconds. Four retries over
// 750 ms let it go first, so a scan does not even count as a failed rotation.
// Elsewhere an open file never blocks either operation, and nothing is retried.
template <typename Op> void retryWhileInUse(Op op, std::error_code& ec)
{
    op(ec);
#ifdef _WIN32
    constexpr int kAccessDenied     = 5;  // ERROR_ACCESS_DENIED: also a pending delete
    constexpr int kSharingViolation = 32; // ERROR_SHARING_VIOLATION
    for (const int ms : {50, 100, 200, 400})
    {
        if (!ec || (ec.value() != kSharingViolation && ec.value() != kAccessDenied))
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        ec.clear();
        op(ec);
    }
#endif
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
// as \u001b, TAB as \t and quotes the double quote, none of which this does.
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

LogFile::LogFile(boost::asio::io_context& ioc,
                 OutputConfig cfg,
                 std::chrono::milliseconds retryInterval)
    : m_cfg(std::move(cfg)), m_strand(boost::asio::make_strand(ioc)),
      m_retryInterval(retryInterval), m_retryTimer(m_strand)
{
}

// closeFiles() must not be called here, because it would race with strand work
// that may still be queued. The std::ofstream members close themselves on
// destruction. For an explicit close, use close(), which posts to the strand.
//
// A compression still running is told to stop and waited for: it holds a
// reference to this object's cancel flag, and m_compressor's own destructor
// would wait for it anyway — only for the whole file rather than one block.
LogFile::~LogFile()
{
    m_cancelCompression = true;
    if (m_compressor)
    {
        m_compressor->join();
    }
}

bool LogFile::openAtStartup()
{
    m_startingUp = true;
    removeStaleCompressions();
    openFiles();
    m_startingUp = false;

    // A shutdown cancels a compression in progress and a crash interrupts one,
    // either way leaving .2 plain — and nothing else would ever compress it,
    // since each rotation compresses only the generation it has just made .2.
    if (m_cfg.compress && !m_closed)
    {
        scheduleCompression();
    }
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
                          m_shuttingDown      = true;
                          m_closed            = true;
                          m_cancelCompression = true;
                          try
                          {
                              // A pending retry is outstanding work, and run()
                              // does not return while there is any: without this
                              // a shutdown during an outage would wait out the
                              // retry interval, and the retry that followed would
                              // reopen files nobody is going to write to.
                              m_retryTimer.cancel();
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
    if (m_startingUp)
    {
        // A sink that cannot be opened at startup is not retried: the caller
        // turns this into EXIT_FAILURE without ever running the io_context, so
        // there is nothing left alive to retry on. Promising one would leave an
        // operator — reading this out of the Windows Event Log, where it is the
        // only trace the process leaves — waiting for a recovery that is not
        // coming. Nor is the reporting policy engaged: there is no second report.
        osLogError("minilog: " + reason + "; sink '" + m_cfg.name + "' cannot be opened");
        m_closed = true;
        closeFiles();
        return;
    }

    const auto decision = m_recovery.onFailure(std::chrono::steady_clock::now());

    if (decision.report)
    {
        if (decision.firstFailure)
        {
            osLogError("minilog: " + reason + "; closing sink '" + m_cfg.name +
                       "', retrying every " + formatInterval(m_retryInterval));
        }
        else
        {
            // The count is of attempts to reopen, so it says the sink is being
            // retried as well as that it is still down — which is what an
            // operator reading this a day into an outage needs to know.
            osLogError("minilog: sink '" + m_cfg.name + "' still closed after " +
                       std::to_string(decision.closedFor.count()) + " s: " + reason + " (" +
                       std::to_string(decision.suppressed) +
                       " failed attempt(s) to reopen it since the last report)");
        }
    }

    m_closed = true;
    closeFiles();
    scheduleRetry();
}

void LogFile::scheduleRetry()
{
    // Belt and braces: no failure can currently reach here after close(), since
    // doWrite() returns early on a closed sink and the retry handler below stops
    // itself. Left in because the cost of a path that did would be a shutdown
    // that waits out the retry interval.
    if (m_shuttingDown)
    {
        return;
    }

    m_retryTimer.expires_after(m_retryInterval);
    m_retryTimer.async_wait(
        [this](const boost::system::error_code& ec)
        {
            // A cancelled timer is a shutdown; m_shuttingDown catches the same
            // thing arriving after this handler was already queued, for which
            // cancel() comes too late.
            if (ec || m_shuttingDown)
            {
                return;
            }
            // Same last line of defence as write(): anything escaping here
            // unwinds out of io_context::run() and takes the process with it.
            try
            {
                attemptReopen();
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

// A rotation that fails part way no longer comes here — it is postponed while
// the sink keeps writing (see rotate()). What does is a failure to open the
// files, including right after a rotation. The retry needs no repair for that:
// openFiles() appends to the active path and creates it when the rename took,
// which is exactly where a successful rotate() would have left it, and the
// sizes it records come from file_size(). So the accounting matches what is on
// disk and the next write rotates from there.
void LogFile::attemptReopen()
{
    // Cleared first so that openFiles() reports a failure through failSink() like
    // any other, which counts the attempt and arms the timer again.
    m_closed = false;
    openFiles();
    if (m_closed)
    {
        return;
    }

    const auto outage = m_recovery.onRecovered(std::chrono::steady_clock::now());
    osLogInfo("minilog: sink '" + m_cfg.name + "' reopened after " +
              std::to_string(outage.closedFor.count()) + " s and " +
              std::to_string(outage.failures) + " failure(s); messages routed to it during that " +
              "time were dropped");
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
    // A rotation that failed is not tried again on every write — each attempt
    // can spend most of a second retrying a file that is in use — but after the
    // retry interval, and the files grow past max_size until then.
    if (m_rotationRetryAt && std::chrono::steady_clock::now() < *m_rotationRetryAt)
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

    waitForCompression();
    closeFiles();

    // Every step below uses the error_code overloads: a throw from here would
    // escape the strand handler and abort the process. shiftFiles stops at the
    // first step that fails and returns why, or "" when it got through.
    //
    // A failure here does not close the sink, unlike a failed write: on Windows
    // another program that merely has one of these files open makes its rename
    // fail, and that is normal rather than broken storage (#50). The rotation is
    // postponed instead and the sink goes on writing to its current files. A
    // shift abandoned partway is safe to resume: it works oldest first and the
    // next attempt probes what is there, so it only ever leaves a gap in the
    // numbering for the next one to close, never two generations out of order.
    auto shiftFiles = [&](const fs::path& base) -> std::string
    {
        std::string failure;

        // exists() with an error_code leaves ec clear for a file that simply is
        // not there, so nullopt means a real failure — a denied or unreachable
        // directory — rather than absence.
        auto probe = [&](const fs::path& p) -> std::optional<bool>
        {
            std::error_code ec;
            const bool found = fs::exists(p, ec);
            if (ec)
            {
                failure = "probing '" + p.string() + "' failed: " + ec.message();
                return std::nullopt;
            }
            return found;
        };

        // A generation is there in either form, plain or compressed. Both are
        // handled whatever `compress` says, so that switching it off does not
        // strand the .gz generations already written. Both forms can also exist
        // at once, briefly, between a compression's rename and its removal of
        // the plain file — or for good, if a crash came in between; they hold
        // the same records and simply move and expire together.
        auto forms = [](const fs::path& plain) { return std::array{plain, gzipPath(plain)}; };

        // Find the highest rotated generation that exists.
        // Probe up to the limit (or a reasonable cap when unlimited) so that
        // gaps left by manually deleted files don't cause orphaned generations.
        const int probeLimit = (m_cfg.maxFiles > 0) ? m_cfg.maxFiles : kMaxFilesLimit;
        int highest          = 0;
        for (int n = 1; n <= probeLimit; ++n)
        {
            for (const auto& p : forms(rotatedPath(base, n)))
            {
                const auto found = probe(p);
                if (!found)
                {
                    return failure;
                }
                if (*found)
                {
                    highest = n;
                }
            }
        }

        // Delete generations beyond max_files.
        if (m_cfg.maxFiles > 0 && highest >= m_cfg.maxFiles)
        {
            for (int n = highest; n >= m_cfg.maxFiles; --n)
            {
                for (const auto& p : forms(rotatedPath(base, n)))
                {
                    const auto found = probe(p);
                    if (!found)
                    {
                        return failure;
                    }
                    if (!*found)
                    {
                        continue;
                    }
                    std::error_code ec;
                    retryWhileInUse([&](std::error_code& e) { fs::remove(p, e); }, ec);
                    if (ec)
                    {
                        return "removing '" + p.string() + "' failed: " + ec.message();
                    }
                }
            }
            highest = m_cfg.maxFiles - 1;
        }

        // Shift existing rotated files up by one.
        for (int n = highest; n >= 1; --n)
        {
            const auto from = forms(rotatedPath(base, n));
            const auto to   = forms(rotatedPath(base, n + 1));
            for (std::size_t i = 0; i < from.size(); ++i)
            {
                const auto found = probe(from[i]);
                if (!found)
                {
                    return failure;
                }
                if (!*found)
                {
                    continue;
                }
                std::error_code ec;
                retryWhileInUse([&](std::error_code& e) { fs::rename(from[i], to[i], e); }, ec);
                if (ec)
                {
                    return "renaming '" + from[i].string() + "' failed: " + ec.message();
                }
            }
        }

        // Rename the current file to .1.
        const auto found = probe(base);
        if (!found)
        {
            return failure;
        }
        if (*found)
        {
            std::error_code ec;
            retryWhileInUse([&](std::error_code& e) { fs::rename(base, rotatedPath(base, 1), e); },
                            ec);
            if (ec)
            {
                return "renaming '" + base.string() + "' failed: " + ec.message();
            }
        }
        return {};
    };

    // The text file first and then the JSONL file, as before. A failure in the
    // first leaves the second alone, so both stay where they were; a failure in
    // the second leaves the first rotated, and the two are a generation apart
    // until the second catches up.
    std::string failure;
    for (const auto& file : {m_cfg.textFile, m_cfg.jsonlFile})
    {
        if (!file.empty())
        {
            failure = shiftFiles(fs::path(file));
            if (!failure.empty())
            {
                break;
            }
        }
    }

    // Opens whatever is at the active paths: a fresh file where the rename
    // went through, the file it had been writing to where it did not. A
    // failure to open is a storage failure like any other and closes the sink.
    openFiles();
    if (m_closed)
    {
        return;
    }

    if (failure.empty())
    {
        rotationSucceeded();
    }
    else
    {
        rotationFailed(failure);
    }

    // Whatever happened, .2 is compressed if it is plain: after a rotation that
    // went through it is the one that has just moved there, and after one that
    // stopped short nothing about it changed.
    if (m_cfg.compress)
    {
        scheduleCompression();
    }
}

void LogFile::rotationFailed(const std::string& reason)
{
    const auto now      = std::chrono::steady_clock::now();
    m_rotationRetryAt   = now + m_retryInterval;
    const auto decision = m_rotationRecovery.onFailure(now);
    if (!decision.report)
    {
        return;
    }
    if (decision.firstFailure)
    {
        osLogError("minilog: sink '" + m_cfg.name + "': rotation failed: " + reason +
                   "; still writing to the current file past max_size, retrying every " +
                   formatInterval(m_retryInterval));
    }
    else
    {
        osLogError("minilog: sink '" + m_cfg.name + "': rotation still failing after " +
                   std::to_string(decision.closedFor.count()) + " s: " + reason + " (" +
                   std::to_string(decision.suppressed) +
                   " failed attempt(s) since the last report)");
    }
}

void LogFile::rotationSucceeded()
{
    if (!m_rotationRetryAt)
    {
        return;
    }
    m_rotationRetryAt.reset();
    const auto outage = m_rotationRecovery.onRecovered(std::chrono::steady_clock::now());
    osLogInfo("minilog: sink '" + m_cfg.name + "' rotated again after " +
              std::to_string(outage.closedFor.count()) + " s and " +
              std::to_string(outage.failures) + " failed attempt(s)");
}

void LogFile::scheduleCompression()
{
    namespace fs = std::filesystem;

    // Called after a rotation, when the generation that has just become .2 was
    // .1 and so is plain, and at startup, for a .2 a shutdown or crash left
    // plain. Older plain generations — left by a failed compression, or from
    // before `compress` was switched on — are left as they are rather than all
    // compressed at once.
    std::vector<fs::path> plain;
    for (const auto& file : {m_cfg.textFile, m_cfg.jsonlFile})
    {
        if (file.empty())
        {
            continue;
        }
        const auto p = rotatedPath(fs::path(file), 2);
        std::error_code ec;
        if (fs::exists(p, ec))
        {
            plain.push_back(p);
        }
    }
    if (plain.empty())
    {
        return;
    }

    std::packaged_task<void()> job(
        [plain = std::move(plain), name = m_cfg.name, &cancel = m_cancelCompression]()
        {
            for (const auto& p : plain)
            {
                const std::string error = gzipFile(p, cancel);
                if (!error.empty())
                {
                    if (cancel)
                    {
                        return; // shutting down; the plain file stays, as intended
                    }
                    osLogError("minilog: sink '" + name + "': compressing '" + p.string() +
                               "' failed: " + error + "; it is kept uncompressed");
                }
            }
        });
    m_compression = job.get_future();
    if (!m_compressor)
    {
        m_compressor.emplace(1);
    }
    boost::asio::post(*m_compressor, std::move(job));
}

void LogFile::waitForCompression()
{
    if (!m_compression.valid())
    {
        return;
    }
    try
    {
        m_compression.get();
    }
    catch (...)
    {
        // The job reports its own failures; what reaches here is one while
        // reporting (out of memory). The plain file is still there either way.
    }
}

void LogFile::removeStaleCompressions()
{
    namespace fs = std::filesystem;

    // One pass over the directory rather than a probe per possible generation,
    // which with max_files = 0 would be a thousand per file. Only names of this
    // sink's own form — "<stem>.<digits><ext>.gz.tmp" — are touched.
    for (const auto& file : {m_cfg.textFile, m_cfg.jsonlFile})
    {
        if (file.empty())
        {
            continue;
        }
        const fs::path base(file);
        const std::string prefix = base.stem().string() + ".";
        const std::string suffix = base.extension().string() + ".gz.tmp";

        std::error_code ec;
        fs::directory_iterator it(base.parent_path(), ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) ||
                !name.ends_with(suffix))
            {
                continue;
            }
            const auto number =
                name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            if (number.find_first_not_of("0123456789") != std::string::npos)
            {
                continue;
            }
            std::error_code removeEc;
            fs::remove(it->path(), removeEc);
        }
    }
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
