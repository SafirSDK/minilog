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

#include "send_options.hpp"

#include "parser/syslog_names.hpp"

#include <boost/program_options.hpp>

#include <cctype>
#include <sstream>

namespace po = boost::program_options;

namespace minilog::send
{

namespace
{

// The option table is built once and shared by the parser and the help text so
// that the two cannot disagree.
po::options_description optionsDescription()
{
    po::options_description desc("Options");
    // clang-format off
    desc.add_options()
        ("help,h",     "show this help and exit")
        ("version",    "print the version and exit")
        ("host",       po::value<std::string>()->default_value("127.0.0.1")->value_name("<host>"),
                       "where to send; a name or an IP address")
        ("port",       po::value<std::string>()->default_value("514")->value_name("<port>"),
                       "UDP port")
        ("facility,f", po::value<std::string>()->default_value("user")->value_name("<name|0-23>"),
                       "syslog facility, by name or number")
        ("severity,s", po::value<std::string>()->default_value("info")->value_name("<name|0-7>"),
                       "syslog severity, by name or number")
        ("app,a",      po::value<std::string>()->default_value("minilog-send")->value_name("<name>"),
                       "APP-NAME (RFC 5424) or tag (RFC 3164)")
        ("hostname",   po::value<std::string>()->value_name("<name>"),
                       "HOSTNAME field; default is this machine's name")
        ("pid",        po::value<std::string>()->value_name("<id>"),
                       "PROCID field; default is this process's pid")
        ("msgid",      po::value<std::string>()->value_name("<id>"),
                       "MSGID field (RFC 5424 only); default none")
        ("rfc3164",    "send the legacy RFC 3164 format instead of RFC 5424")
        ("message",    po::value<std::vector<std::string>>(), "the message");
    // clang-format on
    return desc;
}

// The text of every option but the positional one, which is described in the
// usage line instead.
std::string visibleOptions()
{
    const auto all = optionsDescription();
    po::options_description visible;
    for (const auto& opt : all.options())
    {
        if (opt->long_name() != "message")
        {
            visible.add(opt);
        }
    }
    std::ostringstream os;
    os << visible;
    return os.str();
}

bool allDigits(const std::string& s)
{
    if (s.empty())
    {
        return false;
    }
    for (const unsigned char c : s)
    {
        if (!std::isdigit(c))
        {
            return false;
        }
    }
    return true;
}

// A name from the shared table, or a bare number in [0, max]. Numbers are
// checked here so that "--facility 99" is a usage error with the range in it
// rather than a datagram nobody can decode.
int nameOrNumber(const char* what,
                 const std::string& value,
                 std::optional<int> (*fromName)(std::string_view),
                 int max)
{
    if (allDigits(value))
    {
        // Four digits already exceed any valid value; stoi cannot overflow.
        if (value.size() > 3 || std::stoi(value) > max)
        {
            throw UsageError(std::string(what) + " " + value + " is out of range (0-" +
                             std::to_string(max) + ")");
        }
        return std::stoi(value);
    }
    if (const auto code = fromName(value))
    {
        return *code;
    }
    throw UsageError("unknown " + std::string(what) + " '" + value + "'");
}

std::uint16_t parsePort(const std::string& value)
{
    if (!allDigits(value) || value.size() > 5 || std::stoi(value) < 1 || std::stoi(value) > 65535)
    {
        throw UsageError("port must be a number from 1 to 65535, got '" + value + "'");
    }
    return static_cast<std::uint16_t>(std::stoi(value));
}

} // namespace

std::string usageText()
{
    return "Usage: minilog-send [options] [--] <message words...>\n"
           "       <command> | minilog-send [options]\n"
           "\n"
           "Send a syslog message over UDP. The words on the command line are joined\n"
           "with spaces into one message; with no words, stdin is read to its end and\n"
           "then each non-empty line is sent as its own message.\n"
           "\n" +
           visibleOptions() +
           "\n"
           "Exit status is 0 when every datagram left this machine, 1 when one could\n"
           "not be sent or the host could not be resolved, and 2 for a bad command\n"
           "line. UDP gives no delivery receipt: 0 does not mean the message arrived.\n";
}

SendOptions parseSendOptions(int argc, const char* const argv[])
{
    const auto desc = optionsDescription();
    po::positional_options_description positional;
    positional.add("message", -1);

    po::variables_map vm;
    try
    {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(positional).run(),
                  vm);
        po::notify(vm);
    }
    catch (const po::error& e)
    {
        throw UsageError(e.what());
    }

    SendOptions opts;
    opts.help    = vm.count("help") > 0;
    opts.version = vm.count("version") > 0;
    if (opts.help || opts.version)
    {
        // Nothing else is looked at: "--help --port bogus" still shows the help.
        return opts;
    }

    opts.host = vm["host"].as<std::string>();
    if (opts.host.empty())
    {
        // getaddrinfo takes an empty node as "this machine", so an unset shell
        // variable would quietly log to loopback.
        throw UsageError("--host is empty");
    }
    opts.port = parsePort(vm["port"].as<std::string>());
    opts.facility =
        nameOrNumber("facility", vm["facility"].as<std::string>(), facilityFromName, 23);
    opts.severity = nameOrNumber("severity", vm["severity"].as<std::string>(), severityFromName, 7);
    opts.app      = vm["app"].as<std::string>();
    opts.rfc3164  = vm.count("rfc3164") > 0;
    if (vm.count("hostname"))
    {
        opts.hostname = vm["hostname"].as<std::string>();
    }
    if (vm.count("pid"))
    {
        opts.pid = vm["pid"].as<std::string>();
    }
    if (vm.count("msgid"))
    {
        opts.msgid = vm["msgid"].as<std::string>();
    }

    if (vm.count("message"))
    {
        for (const auto& word : vm["message"].as<std::vector<std::string>>())
        {
            if (!opts.message.empty())
            {
                opts.message += ' ';
            }
            opts.message += word;
        }
        if (opts.message.empty())
        {
            // `minilog-send "$msg"` with $msg unset: an empty word was given,
            // which is not the same as no word, so it must not fall through to
            // reading stdin.
            throw UsageError("the message is empty");
        }
    }
    return opts;
}

} // namespace minilog::send
