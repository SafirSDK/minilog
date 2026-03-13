#define BOOST_TEST_MODULE test_parser
#include <boost/test/unit_test.hpp>
#include "parser/syslog_parser.hpp"
#include "parser/syslog_message.hpp"

using namespace minilog;

// Tests will be implemented in step 4.
// Skeleton placeholder — verifies the parser compiles and returns a result.

BOOST_AUTO_TEST_SUITE(parser_skeleton)

BOOST_AUTO_TEST_CASE(parse_returns_result)
{
    auto msg = parse_syslog("<14>Oct 11 22:14:15 myhost app[123]: hello");
    // Protocol::Unknown until parser is implemented — just check it doesn't crash
    BOOST_TEST(msg.raw == "<14>Oct 11 22:14:15 myhost app[123]: hello");
}

BOOST_AUTO_TEST_SUITE_END()
