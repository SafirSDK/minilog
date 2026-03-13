#include "config.hpp"

namespace minilog {

Config load_config(const std::string& /*path*/)
{
    // TODO: implement INI parsing with Boost.PropertyTree
    return Config{};
}

} // namespace minilog
