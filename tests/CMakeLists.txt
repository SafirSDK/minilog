find_package(Boost REQUIRED COMPONENTS unit_test_framework)

# Helper to register a Boost.Test executable
function(add_boost_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(${name} PRIVATE
        BOOST_TEST_DYN_LINK
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601>
    )
    target_link_libraries(${name} PRIVATE
        Boost::unit_test_framework
        Boost::headers
        Boost::json
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Test executables — each links the sources it needs directly
add_boost_test(test_parser     test_parser.cpp)
target_sources(test_parser     PRIVATE ../src/parser/syslog_parser.cpp)

add_boost_test(test_config     test_config.cpp)
target_sources(test_config     PRIVATE ../src/config/config.cpp)

add_boost_test(test_output     test_output.cpp)
target_sources(test_output     PRIVATE
    ../src/config/config.cpp
    ../src/output/log_file.cpp
    ../src/output/output_manager.cpp
    ../src/parser/syslog_parser.cpp
)

add_boost_test(test_forwarder  test_forwarder.cpp)
target_sources(test_forwarder  PRIVATE
    ../src/config/config.cpp
    ../src/forwarder/forwarder.cpp
    ../src/parser/syslog_parser.cpp
)

add_boost_test(test_integration test_integration.cpp)
target_sources(test_integration PRIVATE
    ../src/config/config.cpp
    ../src/parser/syslog_parser.cpp
    ../src/output/log_file.cpp
    ../src/output/output_manager.cpp
    ../src/forwarder/forwarder.cpp
    ../src/server/udp_server.cpp
)
if(WIN32)
    target_sources(test_integration PRIVATE
        ../src/platform/os_log_win.cpp
        ../src/platform/service_win.cpp
    )
    target_link_libraries(test_integration PRIVATE ws2_32)
else()
    target_sources(test_integration PRIVATE
        ../src/platform/os_log_linux.cpp
        ../src/platform/service_linux.cpp
    )
endif()
