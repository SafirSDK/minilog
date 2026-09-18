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

#include "run_loop.hpp"
#include "udp_server.hpp"

#include "config/config.hpp"
#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"
#include "platform/os_log.hpp"
#include "platform/service.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/program_options.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace po = boost::program_options;

namespace
{

int runServer(const std::string& configPath)
{
    minilog::Config cfg;
    try
    {
        cfg = minilog::loadConfig(configPath);
    }
    catch (const std::exception& e)
    {
        minilog::osLogError(std::string("minilog: failed to load config: ") + e.what());
        return EXIT_FAILURE;
    }

    boost::asio::io_context ioc(cfg.workers);

    minilog::OutputManager outputMgr(ioc, cfg);

    // The only constructor on this path that can throw: it resolves the
    // forwarding endpoint. loadConfig already rejects an unparseable host, so
    // reaching the catch means something else went wrong — which must still be a
    // reported startup failure rather than an abort.
    std::unique_ptr<minilog::Forwarder> forwarder;
    try
    {
        if (cfg.forwarding.enabled)
        {
            forwarder = std::make_unique<minilog::Forwarder>(ioc, cfg.forwarding);
        }
    }
    catch (const std::exception& e)
    {
        minilog::osLogError(std::string("minilog: failed to set up forwarding: ") + e.what());
        outputMgr.close();
        return EXIT_FAILURE;
    }

    minilog::UdpServer server(ioc, cfg, outputMgr, forwarder.get());

    // Install shutdown handler before start() so no signal is missed.
    minilog::setupShutdown(ioc,
                           [&server, &outputMgr]()
                           {
                               server.stop();
                               outputMgr.close();
                           });

    try
    {
        server.start();
    }
    catch (const std::exception& e)
    {
        // Log here rather than inside start(): a throw from before start()'s own
        // error handling used to exit silently, with nothing on stderr and
        // nothing in the Event Log.
        minilog::osLogError(e.what());
        outputMgr.close();
        return EXIT_FAILURE;
    }

    // Everything that can fail at startup has now succeeded, so it is safe to
    // tell the SCM the service is running. Any failure above returns non-zero
    // instead, which the service layer reports to the SCM as a failed start.
    minilog::reportServiceStarted();

    // Spin up workers-1 additional threads; main thread also calls run().
    // Every thread goes through runIoContext: an exception escaping a std::thread's
    // entry function cannot be caught anywhere else, so each thread has to catch
    // its own or the process dies with it.
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cfg.workers - 1));
    for (int i = 0; i < cfg.workers - 1; ++i)
    {
        threads.emplace_back([&ioc]() { minilog::runIoContext(ioc); });
    }
    minilog::runIoContext(ioc);

    for (auto& t : threads)
    {
        t.join();
    }

    return EXIT_SUCCESS;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) — io_context ctor can theoretically throw
// service_already_exists, but only if the same service is registered twice, which never happens
// here.
int main(int argc, char* argv[])
{
    po::options_description desc("Options");
    // clang-format off
    desc.add_options()
        ("help,h",      "show this help message and exit")
        ("config-path", po::value<std::string>(), "path to the configuration file");
#ifdef _WIN32
    desc.add_options()
        ("install",     "install as a Windows service")
        ("uninstall",   "remove the Windows service");
#endif
    // clang-format on

    po::positional_options_description pos;
    pos.add("config-path", 1);

    po::variables_map vm;
    try
    {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), vm);
        po::notify(vm);
    }
    catch (const po::error& e)
    {
        std::cerr << "minilog: " << e.what() << "\n\n" << desc << "\n";
        return EXIT_FAILURE;
    }

    if (vm.count("help"))
    {
        std::cout << "Usage: minilog [options] <config-path>\n\n" << desc << "\n";
        return EXIT_SUCCESS;
    }

    if (vm.count("uninstall"))
    {
        try
        {
            minilog::uninstallService();
        }
        catch (const std::exception& e)
        {
            minilog::osLogError(e.what());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (!vm.count("config-path"))
    {
        std::cerr << "minilog: config-path is required\n\n" << desc << "\n";
        return EXIT_FAILURE;
    }

    const std::string configPath = vm["config-path"].as<std::string>();

    if (vm.count("install"))
    {
        try
        {
            // absolute() throws; inside the try it is reported like any other
            // install failure instead of escaping main.
            const std::string exePath = std::filesystem::absolute(argv[0]).string();
            minilog::installService(exePath, configPath);
        }
        catch (const std::exception& e)
        {
            minilog::osLogError(e.what());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    // Try to start as a Windows NT service. If this process was launched by
    // the SCM, StartServiceCtrlDispatcher will dispatch into runServer() and
    // this call will not return until the service exits.
    if (const auto exitCode =
            minilog::tryRunAsService([&configPath]() { return runServer(configPath); }))
    {
        return *exitCode;
    }

    // Running interactively (console process or Linux).
    return runServer(configPath);
}
