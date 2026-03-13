#define BOOST_TEST_MODULE test_config
#include <boost/test/unit_test.hpp>
#include "config/config.hpp"

using namespace minilog;

// Tests will be implemented in step 3.

BOOST_AUTO_TEST_SUITE(config_skeleton)

BOOST_AUTO_TEST_CASE(default_config_compiles)
{
    Config cfg;
    BOOST_TEST(cfg.udp_port == 514);
    BOOST_TEST(cfg.workers  == 4);
}

BOOST_AUTO_TEST_SUITE_END()
