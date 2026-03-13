#pragma once
#include <string>

namespace minilog
{

// Log a message to the OS facility (Windows Event Log or Linux syslog) and stderr.
void os_log_error(const std::string& message);
void os_log_info(const std::string& message);

} // namespace minilog
