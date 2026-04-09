cmake_minimum_required(VERSION 3.25)
project(minilog VERSION 1.2.0 LANGUAGES CXX)

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

# Duration (seconds) of each soak test when MINILOG_STRESS_LOSSY=1.
set(MINILOG_STRESS_SOAK_SECONDS "10" CACHE STRING
    "Soak window in seconds per lossy test case (default 10, extended presets use 1800)")


if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clang-tidy and editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Place all runtime binaries in a single bin/ subdirectory so that target
# names never collide with output file paths (which would confuse Ninja).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Treat warnings as errors project-wide.
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
add_compile_options(
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>
)

find_package(Boost REQUIRED COMPONENTS json program_options unit_test_framework)

# Platform-specific sources
if(WIN32)
    find_program(MC_EXECUTABLE mc REQUIRED)
    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/messages.h
            ${CMAKE_CURRENT_BINARY_DIR}/messages.rc
            ${CMAKE_CURRENT_BINARY_DIR}/MSG00001.bin
        COMMAND ${MC_EXECUTABLE}
            -h ${CMAKE_CURRENT_BINARY_DIR}
            -r ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server/platform/messages.mc
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/server/platform/messages.mc
        COMMENT "Compiling Windows message table..."
    )
    # Named target so test executables can add_dependencies() on the generated header.
    add_custom_target(minilog_generate_messages
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/messages.h
    )

    set(PLATFORM_SOURCES
        src/server/platform/os_log_win.cpp
        src/server/platform/service_win.cpp
        src/server/minilog.rc
        ${CMAKE_CURRENT_BINARY_DIR}/messages.rc
    )
    set_source_files_properties(
        ${CMAKE_CURRENT_BINARY_DIR}/messages.rc
        PROPERTIES COMPILE_FLAGS "/I\"${CMAKE_CURRENT_BINARY_DIR}\""
    )
else()
    set(PLATFORM_SOURCES
        src/server/platform/os_log_linux.cpp
        src/server/platform/service_linux.cpp
    )
endif()

add_executable(minilog
    src/server/main.cpp
    src/server/config/config.cpp
    src/server/parser/syslog_parser.cpp
    src/server/udp_server.cpp
    src/server/output/output_manager.cpp
    src/server/output/log_file.cpp
    src/server/forwarder/forwarder.cpp
    ${PLATFORM_SOURCES}
)

target_include_directories(minilog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/server)

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
    Boost::program_options
)

if(WIN32)
    target_link_libraries(minilog PRIVATE ws2_32 advapi32)
    target_include_directories(minilog PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Go web-viewer
# ---------------------------------------------------------------------------
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
    set(_web_viewer_src_dir "${CMAKE_SOURCE_DIR}/src/web-viewer")
    set(_web_viewer_bin
        "${CMAKE_BINARY_DIR}/bin/minilog-web-viewer${CMAKE_EXECUTABLE_SUFFIX}")

    # Collect all Go source files and embedded assets as dependencies so
    # CMake re-runs go build whenever any of them change.
    file(GLOB_RECURSE _web_viewer_go_sources
        "${_web_viewer_src_dir}/*.go"
        "${_web_viewer_src_dir}/go.mod"
        "${_web_viewer_src_dir}/go.sum"
        "${_web_viewer_src_dir}/assets/*.html"
        "${_web_viewer_src_dir}/assets/*.js"
        "${_web_viewer_src_dir}/assets/*.css"
    )

    add_custom_command(
        OUTPUT "${_web_viewer_bin}"
        COMMAND "${GO_EXECUTABLE}" build
            -v
            -mod vendor
            -ldflags "-X main.version=${PROJECT_VERSION}"
            -o "${_web_viewer_bin}"
            ./
        DEPENDS ${_web_viewer_go_sources}
        WORKING_DIRECTORY "${_web_viewer_src_dir}"
        COMMENT "Building minilog-web-viewer (Go)..."
        VERBATIM
    )

    add_custom_target(minilog-web-viewer ALL
        DEPENDS "${_web_viewer_bin}"
    )
else()
    message(WARNING "Go compiler not found — minilog-web-viewer will not be built. "
                    "Install Go and re-run cmake to enable the web viewer.")
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS minilog
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(PROGRAMS
    "${CMAKE_SOURCE_DIR}/src/cli-viewer/minilog-cli-viewer.py"
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(GO_EXECUTABLE)
    install(PROGRAMS
        "${_web_viewer_bin}"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()

# ---------------------------------------------------------------------------
# Inno Setup installer (Windows only).
# ---------------------------------------------------------------------------
if(WIN32)
    find_program(ISCC ISCC HINTS
        "C:/Program Files (x86)/Inno Setup 6"
        "C:/Program Files/Inno Setup 6"
    )
    if(ISCC)
        # Collect the DEPENDS list: always the server, plus the web viewer if Go
        # is available.
        set(_package_deps minilog)
        set(_iscc_web_viewer_args "")
        if(GO_EXECUTABLE)
            list(APPEND _package_deps minilog-web-viewer)
            list(APPEND _iscc_web_viewer_args
                "/DWebViewerDir=${CMAKE_BINARY_DIR}/bin")
        endif()

        add_custom_target(package
            COMMAND "${ISCC}"
                "/DSourceDir=$<TARGET_FILE_DIR:minilog>"
                "/DConfigDir=${CMAKE_SOURCE_DIR}/installer"
                "/DAppVersion=${PROJECT_VERSION}"
                "/DOutputDir=${CMAKE_BINARY_DIR}"
                ${_iscc_web_viewer_args}
                "${CMAKE_SOURCE_DIR}/installer/setup.iss"
            DEPENDS ${_package_deps}
            COMMENT "Building Inno Setup installer..."
            VERBATIM
        )
    endif()
endif()
