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

#include "compress.hpp"

#include <fstream>
#include <system_error>
#include <vector>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace minilog
{

namespace
{

namespace fs = std::filesystem;

// Flush a closed file's data to the disk, so that the rename which follows
// cannot reach the disk before the bytes it names do. Without it a power loss
// shortly after a compression could leave an empty or partial .gz in place of
// a plain file that has already been removed.
bool syncToDisk(const fs::path& path)
{
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const bool ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

gzFile openGzip(const fs::path& path)
{
    // Level 6, zlib's default: about 13x on JSONL, against 9x at level 1, and
    // it runs off the write path, so the extra time costs nothing a sender sees.
#ifdef _WIN32
    return gzopen_w(path.c_str(), "wb6");
#else
    return gzopen(path.c_str(), "wb6");
#endif
}

// Write `plain` gzipped into `tmp`. Returns what went wrong, or "".
std::string writeGzip(const fs::path& plain, const fs::path& tmp, const std::atomic<bool>& cancel)
{
    std::ifstream in(plain, std::ios::binary);
    if (!in)
    {
        return "cannot open '" + plain.string() + "'";
    }

    gzFile out = openGzip(tmp);
    if (out == nullptr)
    {
        return "cannot create '" + tmp.string() + "'";
    }

    std::vector<char> buffer(256 * 1024);
    std::string error;
    while (error.empty())
    {
        if (cancel)
        {
            error = "cancelled by shutdown";
            break;
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<unsigned>(in.gcount());
        if (got > 0 && gzwrite(out, buffer.data(), got) != static_cast<int>(got))
        {
            int code       = 0;
            const char* gz = gzerror(out, &code);
            error = "write to '" + tmp.string() + "' failed: " + (gz != nullptr ? gz : "unknown");
            break;
        }
        if (in.eof())
        {
            break;
        }
        if (!in)
        {
            error = "read from '" + plain.string() + "' failed";
        }
    }

    // gzclose writes the trailer, so a full disk can surface only here.
    if (gzclose(out) != Z_OK && error.empty())
    {
        error = "finishing '" + tmp.string() + "' failed";
    }
    if (error.empty() && !syncToDisk(tmp))
    {
        error = "syncing '" + tmp.string() + "' to disk failed";
    }
    return error;
}

} // namespace

fs::path gzipPath(const fs::path& plain)
{
    fs::path p = plain;
    p += ".gz";
    return p;
}

fs::path gzipTempPath(const fs::path& plain)
{
    fs::path p = plain;
    p += ".gz.tmp";
    return p;
}

std::string gzipFile(const fs::path& plain, const std::atomic<bool>& cancel)
{
    try
    {
        const fs::path tmp = gzipTempPath(plain);
        std::string error  = writeGzip(plain, tmp, cancel);

        std::error_code ec;
        if (error.empty())
        {
            fs::rename(tmp, gzipPath(plain), ec);
            if (ec)
            {
                error = "rename to '" + gzipPath(plain).string() + "' failed: " + ec.message();
            }
        }
        if (!error.empty())
        {
            fs::remove(tmp, ec);
            return error;
        }

        // Both now exist with the same records; the plain one goes last.
        fs::remove(plain, ec);
        if (ec)
        {
            return "compressed, but removing '" + plain.string() + "' failed: " + ec.message() +
                   " (both copies are kept)";
        }
        return {};
    }
    catch (const std::exception& e)
    {
        return std::string("unexpected exception: ") + e.what();
    }
    catch (...)
    {
        return "unexpected non-standard exception";
    }
}

} // namespace minilog
