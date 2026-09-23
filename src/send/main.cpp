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

// minilog-send: build a syslog datagram from the command line (or from stdin,
// one per line) and send it over UDP. Exists because Windows has no logger(1),
// so a script there had no simple way to put a line into minilog, and because
// sending one message and watching it arrive is the end-to-end check that
// `minilog --check` deliberately stops short of.
//
// Nothing here writes to the Windows Event Log or reads minilog.conf: errors go
// to stderr, and the destination is whatever the flags say.

#include "send_options.hpp"
#include "syslog_format.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/udp.hpp>

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef MINILOG_SEND_VERSION
#define MINILOG_SEND_VERSION "unknown"
#endif

namespace
{

// Exit codes, as documented in --help: a script can tell "you asked for
// something impossible" (2) from "it did not go" (1).
constexpr int kExitSendFailed = 1;
constexpr int kExitUsage      = 2;

std::string ownPid()
{
#ifdef _WIN32
    return std::to_string(GetCurrentProcessId());
#else
    return std::to_string(getpid());
#endif
}

class Sender
{
public:
    Sender(const std::string& host, std::uint16_t port)
    {
        boost::asio::ip::udp::resolver resolver(m_ioc);
        boost::system::error_code ec;
        const auto results = resolver.resolve(host, std::to_string(port), ec);
        if (ec || results.empty())
        {
            throw std::runtime_error("cannot resolve '" + host +
                                     "': " + (ec ? ec.message() : std::string("no addresses")));
        }
        m_endpoint = results.begin()->endpoint();
        m_socket.open(m_endpoint.protocol());
    }

    void send(const std::string& datagram)
    {
        boost::system::error_code ec;
        m_socket.send_to(boost::asio::buffer(datagram), m_endpoint, 0, ec);
        if (ec)
        {
            throw std::runtime_error("send to " + m_endpoint.address().to_string() + ":" +
                                     std::to_string(m_endpoint.port()) +
                                     " failed: " + ec.message());
        }
    }

private:
    boost::asio::io_context m_ioc;
    boost::asio::ip::udp::socket m_socket{m_ioc};
    boost::asio::ip::udp::endpoint m_endpoint;
};

// Format one message with the time now, refusing what cannot be sent. Throws
// std::runtime_error naming the problem.
std::string
buildDatagram(const minilog::send::SyslogFields& base, const std::string& message, bool rfc3164)
{
    using namespace minilog::send;
    SyslogFields f = base;
    f.message      = message;
    const auto now = localNow();
    f.timestamp    = rfc3164 ? rfc3164Timestamp(now) : rfc5424Timestamp(now);
    validateFields(f, rfc3164);

    std::string datagram = rfc3164 ? formatRfc3164(f) : formatRfc5424(f);
    if (datagram.size() > kMaxDatagramBytes)
    {
        throw std::runtime_error("the message is " + std::to_string(datagram.size()) +
                                 " bytes with its header; a UDP datagram holds at most " +
                                 std::to_string(kMaxDatagramBytes));
    }
    return datagram;
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace minilog::send;

    SendOptions opts;
    try
    {
        opts = parseSendOptions(argc, argv);
    }
    catch (const UsageError& e)
    {
        std::cerr << "minilog-send: " << e.what() << "\nTry 'minilog-send --help'.\n";
        return kExitUsage;
    }

    if (opts.help)
    {
        std::cout << usageText();
        return EXIT_SUCCESS;
    }
    if (opts.version)
    {
        std::cout << "minilog-send " MINILOG_SEND_VERSION "\n";
        return EXIT_SUCCESS;
    }

    SyslogFields fields;
    fields.facility = opts.facility;
    fields.severity = opts.severity;
    fields.app      = opts.app;
    fields.pid      = opts.pid ? opts.pid : std::optional<std::string>(ownPid());
    fields.msgid    = opts.msgid;
    try
    {
        fields.hostname = opts.hostname ? *opts.hostname : boost::asio::ip::host_name();
    }
    catch (const std::exception& e)
    {
        std::cerr << "minilog-send: cannot read this machine's hostname (" << e.what()
                  << "); pass --hostname\n";
        return kExitSendFailed;
    }

    // The header fields and the command-line message are checked before
    // anything is resolved or opened, so that a bad flag is a usage error and
    // nothing else — in stdin mode too, where the datagrams are built later.
    std::string single;
    try
    {
        if (!opts.message.empty())
        {
            single = buildDatagram(fields, opts.message, opts.rfc3164);
        }
        else
        {
            SyslogFields probe = fields;
            probe.message      = "x";
            validateFields(probe, opts.rfc3164);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "minilog-send: " << e.what() << "\nTry 'minilog-send --help'.\n";
        return kExitUsage;
    }

    try
    {
        Sender sender(opts.host, opts.port);

        if (!single.empty())
        {
            sender.send(single);
            return EXIT_SUCCESS;
        }

        // stdin mode. The whole input is read first: a message per line means
        // the split has to see line ends, and a pipe delivers them in whatever
        // pieces it likes.
#ifdef _WIN32
        // The CRT opens stdin in text mode, where a 0x1A byte is end of file
        // and everything after it would be dropped without a word. The message
        // is bytes to this tool; splitLines handles CRLF on its own.
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        const std::string input((std::istreambuf_iterator<char>(std::cin)),
                                std::istreambuf_iterator<char>());
        const auto lines = splitLines(input);
        if (lines.empty())
        {
            std::cerr << "minilog-send: no message given and nothing on stdin\n"
                         "Try 'minilog-send --help'.\n";
            return kExitUsage;
        }
        std::size_t sent = 0;
        for (const auto& line : lines)
        {
            // A bad line stops the run rather than being skipped: the exit
            // code then says something was not sent, and stderr says which.
            try
            {
                sender.send(buildDatagram(fields, line, opts.rfc3164));
            }
            catch (const std::exception& e)
            {
                std::cerr << "minilog-send: line " << (sent + 1) << ": " << e.what() << " (" << sent
                          << " of " << lines.size() << " sent)\n";
                return kExitSendFailed;
            }
            ++sent;
        }
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cerr << "minilog-send: " << e.what() << "\n";
        return kExitSendFailed;
    }
}
