cmake_minimum_required(VERSION 3.25)
project(minilog VERSION 0.1.0 LANGUAGES CXX)

# Scale factor for stress test message counts.
# Full load (100) is used in normal builds.  Instrumented builds (coverage,
# sanitizers) should use a smaller value (e.g. 10) so tests complete in time.
set(MINILOG_STRESS_MULTIPLIER "100" CACHE STRING
    "Stress test scale factor: flood = 100×N, concurrent = 10×N messages per thread")

# When ON, the soak test suites are compiled in and count-sensitive assertions
# in the flood/concurrent tests are replaced by a fixed soak window.  Intended
# for the extended sanitizer presets where intentional UDP overload is expected.
set(MINILOG_STRESS_LOSSY "0" CACHE STRING
    "Set to 1 to enable lossy soak tests (extended ASan/TSan presets)")

cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treat warnings as errors project-wide.
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
add_compile_options(
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>
)

find_package(Boost REQUIRED COMPONENTS json unit_test_framework)

# Platform-specific sources
if(WIN32)
    set(PLATFORM_SOURCES
        src/platform/os_log_win.cpp
        src/platform/service_win.cpp
    )
else()
    set(PLATFORM_SOURCES
        src/platform/os_log_linux.cpp
        src/platform/service_linux.cpp
    )
endif()

add_executable(minilog
    src/main.cpp
    src/config/config.cpp
    src/parser/syslog_parser.cpp
    src/server/udp_server.cpp
    src/output/output_manager.cpp
    src/output/log_file.cpp
    src/forwarder/forwarder.cpp
    ${PLATFORM_SOURCES}
)

target_include_directories(minilog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(minilog PRIVATE
    BOOST_ASIO_NO_DEPRECATED
    $<$<PLATFORM_ID:Windows>:
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    >
)

target_link_libraries(minilog PRIVATE
    Boost::headers
    Boost::json
)

if(WIN32)
    target_link_libraries(minilog PRIVATE ws2_32 advapi32)
endif()

enable_testing()
add_subdirectory(tests)

# Inno Setup installer (Windows only).
if(WIN32)
    find_program(ISCC ISCC HINTS
        "C:/Program Files (x86)/Inno Setup 6"
        "C:/Program Files/Inno Setup 6"
    )
    if(ISCC)
        add_custom_target(package
            COMMAND "${ISCC}"
                "/DSourceDir=$<TARGET_FILE_DIR:minilog>"
                "/DConfigDir=${CMAKE_SOURCE_DIR}/installer"
                "/DAppVersion=${PROJECT_VERSION}"
                "/DOutputDir=${CMAKE_BINARY_DIR}"
                "${CMAKE_SOURCE_DIR}/installer/setup.iss"
            DEPENDS minilog
            COMMENT "Building Inno Setup installer..."
            VERBATIM
        )
    endif()
endif()

# clang-format compliance check (warning only, does not break the build).
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(clang-format-check ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DCLANG_FORMAT=${CLANG_FORMAT}"
            "-DSRC_DIR=${CMAKE_SOURCE_DIR}/src"
            "-DTEST_DIR=${CMAKE_SOURCE_DIR}/tests"
            -P "${CMAKE_SOURCE_DIR}/cmake/clang_format_check.cmake"
        COMMENT "Checking clang-format..."
        VERBATIM
    )
endif()
