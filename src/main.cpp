#include "config/config.hpp"
#include "forwarder/forwarder.hpp"
#include "output/output_manager.hpp"
#include "platform/os_log.hpp"
#include "server/udp_server.hpp"

#include <boost/asio/io_context.hpp>

#include <thread>
#include <vector>

int main(int /*argc*/, char* /*argv*/[]) {
    // TODO: parse CLI args (--install, --uninstall, config path)
    // TODO: detect Windows service vs console mode
    // TODO: load config
    // TODO: set up io_context thread pool, OutputManager, Forwarder, UdpServer
    // TODO: run until signal
    return 0;
}
