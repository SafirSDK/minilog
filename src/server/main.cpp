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

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace po = boost::program_options;

namespace
{

// How long --stop and --uninstall wait for the service process to go away.
// Generous: the cost of waiting a little longer is nothing next to the cost of
// concluding too early that the executable is free to overwrite.
constexpr int DEFAULT_STOP_TIMEOUT_S = 30;

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

    // Open the sinks before reporting the service running, so an unwritable log
    // path fails the start instead of surfacing on the first message — by which
    // point the SCM has long since been told the service is healthy.
    if (!outputMgr.open())
    {
        // Each failure was already reported by name inside failSink().
        return EXIT_FAILURE;
    }

    // The only constructor on this path that can throw. It resolves the
    // forwarding destination, but a name that does not resolve is reported and
    // retried rather than thrown — a service started before DNS is up must not
    // fail its start over it — so reaching the catch means something else went
    // wrong, which must still be a reported startup failure rather than an
    // abort.
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
    // The forwarder is stopped too: an unresolved destination leaves a retry
    // timer outstanding, and io_context::run() does not return while it is
    // pending — minilog would take the whole retry delay to exit, or never.
    minilog::setupShutdown(ioc,
                           [&server, &outputMgr, &forwarder]()
                           {
                               server.stop();
                               outputMgr.close();
                               if (forwarder)
                               {
                                   forwarder->stop();
                               }
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
    std::atomic<bool> handlerThrew{false};
    const auto runWorker = [&ioc, &handlerThrew]()
    {
        if (!minilog::runIoContext(ioc))
        {
            handlerThrew = true;
        }
    };

    // loadConfig caps workers, so exhausting the thread limit here means the
    // host is genuinely out of resources rather than the config being wrong.
    // Either way it must be reported: an escaping std::system_error reaches
    // std::terminate, which on Windows means a core dump and an empty Event Log.
    // The threads already started keep running — ingestion on fewer workers than
    // asked for beats no ingestion at all.
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cfg.workers - 1));
    try
    {
        for (int i = 0; i < cfg.workers - 1; ++i)
        {
            threads.emplace_back(runWorker);
        }
    }
    catch (const std::exception& e)
    {
        minilog::osLogError("minilog: could not start all " + std::to_string(cfg.workers) +
                            " workers (" + e.what() + "); continuing with " +
                            std::to_string(threads.size() + 1));
    }
    runWorker();

    for (auto& t : threads)
    {
        t.join();
    }

    // A caught handler exception is not a clean shutdown: reporting it as one
    // would leave a service that has stopped ingesting looking like a service
    // that was asked to stop.
    return handlerThrew ? EXIT_FAILURE : EXIT_SUCCESS;
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
        ("stop",        "stop the Windows service and wait for its process to exit")
        ("uninstall",   "remove the Windows service")
        ("timeout",     po::value<int>()->default_value(DEFAULT_STOP_TIMEOUT_S),
                        "seconds to wait for --stop and --uninstall");
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

    if (vm.count("stop") || vm.count("uninstall"))
    {
        // default_value only applies where the option exists, i.e. on Windows.
        const int timeout = vm.count("timeout") ? vm["timeout"].as<int>() : DEFAULT_STOP_TIMEOUT_S;
        if (timeout <= 0)
        {
            std::cerr << "minilog: --timeout must be at least 1 second\n";
            return EXIT_FAILURE;
        }

        try
        {
            if (vm.count("uninstall"))
            {
                minilog::uninstallService(std::chrono::seconds(timeout));
            }
            else
            {
                minilog::stopService(std::chrono::seconds(timeout));
            }
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
            // The SCM starts services with the working directory set to
            // System32, so a relative config path would resolve there at boot.
            // Absolutising against the current directory is right here and only
            // here: --install is run interactively, where the CWD is the user's.
            // absolute() throws; inside the try it is reported like any other
            // install failure instead of escaping main.
            const auto absConfig = std::filesystem::absolute(configPath).lexically_normal();

            // A service registered against a config it cannot read fails at
            // every boot and succeeds at install time — refusing to register is
            // the more useful of the two.
            const std::ifstream probe(absConfig);
            if (!probe.good())
            {
                throw std::runtime_error("minilog: cannot read config file " + absConfig.string() +
                                         " — service not installed");
            }

            minilog::installService(absConfig.string());
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
