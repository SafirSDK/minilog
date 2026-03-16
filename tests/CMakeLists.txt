find_package(Boost REQUIRED COMPONENTS unit_test_framework)

# Helper to register a Boost.Test executable
function(add_boost_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(${name} PRIVATE
        # DYN_LINK only on Linux where system Boost is a shared library;
        # on Windows Conan provides a static lib and this macro must be absent.
        $<$<NOT:$<PLATFORM_ID:Windows>>:BOOST_TEST_DYN_LINK>
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
if(WIN32)
    target_sources(test_output PRIVATE ../src/platform/os_log_win.cpp)
    target_include_directories(test_output PRIVATE ${CMAKE_BINARY_DIR})
    add_dependencies(test_output minilog_generate_messages)
else()
    target_sources(test_output PRIVATE ../src/platform/os_log_linux.cpp)
endif()

add_boost_test(test_forwarder  test_forwarder.cpp)
target_sources(test_forwarder  PRIVATE
    ../src/config/config.cpp
    ../src/forwarder/forwarder.cpp
    ../src/parser/syslog_parser.cpp
)
if(WIN32)
    target_sources(test_forwarder PRIVATE ../src/platform/os_log_win.cpp)
    target_include_directories(test_forwarder PRIVATE ${CMAKE_BINARY_DIR})
    add_dependencies(test_forwarder minilog_generate_messages)
else()
    target_sources(test_forwarder PRIVATE ../src/platform/os_log_linux.cpp)
endif()

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
    target_include_directories(test_integration PRIVATE ${CMAKE_BINARY_DIR})
    add_dependencies(test_integration minilog_generate_messages)
    target_link_libraries(test_integration PRIVATE ws2_32)
else()
    target_sources(test_integration PRIVATE
        ../src/platform/os_log_linux.cpp
        ../src/platform/service_linux.cpp
    )
endif()

add_boost_test(test_stress test_stress.cpp)
target_compile_definitions(test_stress PRIVATE
    MINILOG_STRESS_MULTIPLIER=${MINILOG_STRESS_MULTIPLIER}
    MINILOG_STRESS_LOSSY=${MINILOG_STRESS_LOSSY}
    MINILOG_STRESS_SOAK_SECONDS=${MINILOG_STRESS_SOAK_SECONDS})
if(MINILOG_STRESS_LOSSY)
    # Each soak test case runs for SOAK_SECONDS; two cases = 2×SOAK_SECONDS minimum.
    math(EXPR _soak_timeout "${MINILOG_STRESS_SOAK_SECONDS} * 3")
    set_tests_properties(test_stress PROPERTIES TIMEOUT ${_soak_timeout})
endif()
target_sources(test_stress PRIVATE
    ../src/config/config.cpp
    ../src/parser/syslog_parser.cpp
    ../src/output/log_file.cpp
    ../src/output/output_manager.cpp
    ../src/forwarder/forwarder.cpp
    ../src/server/udp_server.cpp
)
if(WIN32)
    target_sources(test_stress PRIVATE
        ../src/platform/os_log_win.cpp
        ../src/platform/service_win.cpp
    )
    target_include_directories(test_stress PRIVATE ${CMAKE_BINARY_DIR})
    add_dependencies(test_stress minilog_generate_messages)
    target_link_libraries(test_stress PRIVATE ws2_32)
else()
    target_sources(test_stress PRIVATE
        ../src/platform/os_log_linux.cpp
        ../src/platform/service_linux.cpp
    )
endif()

# Binary-level tests — require the minilog executable.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME test_binary
        COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/test_binary.py
            $<TARGET_FILE:minilog>
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(test_binary PROPERTIES TIMEOUT 120)
endif()

# libFuzzer target (Clang only, enabled via linux-fuzz preset).
option(MINILOG_FUZZ "Build libFuzzer fuzz_parser target" OFF)
if(MINILOG_FUZZ)
    add_executable(fuzz_parser
        fuzz/fuzz_parser.cpp
        ../src/parser/syslog_parser.cpp
    )
    target_include_directories(fuzz_parser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address)
    target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address)
endif()

# clang-tidy static analysis.
# Run with: cmake --build <build-dir> --target clang-tidy-check
find_program(RUN_CLANG_TIDY NAMES
    run-clang-tidy
    run-clang-tidy-21 run-clang-tidy-20 run-clang-tidy-19
    run-clang-tidy-18 run-clang-tidy-17 run-clang-tidy-16
)
if(RUN_CLANG_TIDY)
    # Under Nix (and similar wrapper-based toolchains) the compiler wrapper
    # injects system -isystem paths at runtime; these are not captured in
    # compile_commands.json.  Pass them explicitly so clang-tidy's built-in
    # Clang frontend can find standard headers.
    set(_tidy_extra_args "")
    foreach(_dir ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
        list(APPEND _tidy_extra_args "-extra-arg=-isystem${_dir}")
    endforeach()

    add_custom_target(clang-tidy-check
        COMMAND "${RUN_CLANG_TIDY}"
            -p "${CMAKE_BINARY_DIR}"
            ${_tidy_extra_args}
            "${CMAKE_SOURCE_DIR}/src"
        COMMENT "Running clang-tidy..."
        VERBATIM
    )
endif()

# clang-format compliance check (warning only, does not break the build).
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(clang-format-check ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DCLANG_FORMAT=${CLANG_FORMAT}"
            "-DSRC_DIR=${CMAKE_SOURCE_DIR}/src"
            "-DTEST_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/clang_format_check.cmake"
        COMMENT "Checking clang-format..."
        VERBATIM
    )
endif()
