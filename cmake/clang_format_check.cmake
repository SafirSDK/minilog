# Invoked at build time by the clang-format-check target.
# Scans src/ and tests/ for formatting issues and emits a CMake warning
# (non-fatal) listing any non-conformant files.
#
# Required variables (passed via -D):
#   CLANG_FORMAT  - path to the clang-format executable
#   SRC_DIR       - path to the src/ directory
#   TEST_DIR      - path to the tests/ directory

file(GLOB_RECURSE sources
    "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.hpp"
    "${TEST_DIR}/*.cpp" "${TEST_DIR}/*.hpp"
)

set(bad_files "")
foreach(f IN LISTS sources)
    execute_process(
        COMMAND "${CLANG_FORMAT}" --dry-run --Werror "${f}"
        RESULT_VARIABLE result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT result EQUAL 0)
        list(APPEND bad_files "${f}")
    endif()
endforeach()

if(bad_files)
    list(JOIN bad_files "\n  " bad_files_str)
    message(WARNING
        "clang-format: the following files are not formatted correctly:\n"
        "  ${bad_files_str}\n"
        "Run: clang-format -i <file>  (or: clang-format -i src/**/*.cpp src/**/*.hpp tests/**/*.cpp)")
endif()
