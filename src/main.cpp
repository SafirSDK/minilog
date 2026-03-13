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

#include "config/config.hpp"
#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"
#include "platform/os_log.hpp"
#include "platform/service.hpp"
#include "server/udp_server.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

void printUsage(const char* prog)
{
    std::cerr << "Usage:\n"
              << "  " << prog << " <config-path>              run (interactive or service)\n"
              << "  " << prog << " --install <config-path>    install Windows service\n"
              << "  " << prog << " --uninstall                remove Windows service\n";
}

int runServer(const std::string& configPath)
{
    minilog::Config cfg;
    try
    {
        cfg = minilog::loadConfig(configPath);
    }
    catch (const std::exception& e)
    {
        minilog::os_log_error(std::string("minilog: failed to load config: ") + e.what());
        return EXIT_FAILURE;
    }

    boost::asio::io_context ioc(cfg.workers);

    minilog::OutputManager outputMgr(ioc, cfg);

    std::unique_ptr<minilog::Forwarder> forwarder;
    if (cfg.forwarding.enabled)
    {
        forwarder = std::make_unique<minilog::Forwarder>(ioc, cfg.forwarding);
    }

    minilog::UdpServer server(ioc, cfg, outputMgr, forwarder.get());

    try
    {
        server.start();
    }
    catch (const std::exception& /*e*/)
    {
        // os_log_error already called inside start(); just exit.
        outputMgr.close();
        return EXIT_FAILURE;
    }

    minilog::setupShutdown(ioc,
                           [&server, &outputMgr]()
                           {
                               server.stop();
                               outputMgr.close();
                           });

    // Spin up workers-1 additional threads; main thread also calls run().
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cfg.workers - 1));
    for (int i = 0; i < cfg.workers - 1; ++i)
    {
        threads.emplace_back([&ioc]() { ioc.run(); });
    }
    ioc.run();

    for (auto& t : threads)
    {
        t.join();
    }

    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string(argv[1]) == "--uninstall")
    {
        try
        {
            minilog::uninstallService();
        }
        catch (const std::exception& e)
        {
            minilog::os_log_error(e.what());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (argc == 3 && std::string(argv[1]) == "--install")
    {
        const std::string configPath = argv[2];
        const std::string exePath    = std::filesystem::absolute(argv[0]).string();
        try
        {
            minilog::installService(exePath, configPath);
        }
        catch (const std::exception& e)
        {
            minilog::os_log_error(e.what());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (argc != 2)
    {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string configPath = argv[1];

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
