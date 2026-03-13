#include "os_log.hpp"

#include <iostream>
#include <syslog.h>

namespace minilog
{

void os_log_error(const std::string& message)
{
    syslog(LOG_ERR, "%s", message.c_str());
    std::cerr << "[ERROR] " << message << "\n";
}

void os_log_info(const std::string& message)
{
    syslog(LOG_INFO, "%s", message.c_str());
    std::cerr << "[INFO] " << message << "\n";
}

} // namespace minilog
