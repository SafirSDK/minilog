# Assemble the zip archive for installing minilog without the installer.
#
# Run by the `package-zip` target in the top-level CMakeLists.txt as
#   cmake -DVERSION=... -DSOURCE_DIR=... -DOUTPUT_DIR=... -DSERVER_EXE=...
#         -DSEND_EXE=... [-DSERVER_PDB=...] [-DWEB_VIEWER_EXE=...]
#         -P cmake/package_zip.cmake
#
# The archive holds one directory, minilog-<version>/, with the same files the
# installer lays down — the three executables, the default config, the CLI
# viewer and its config — plus the licence, README and changelog. Nothing in it is
# generated: an administrator copies the files where they want them and follows
# the README's "Windows deployment without the installer" section. The list of
# files is checked by tests/installer/test_zip_install.py, so a change here is a
# change there and in the README.

foreach(var VERSION SOURCE_DIR OUTPUT_DIR SERVER_EXE SEND_EXE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "package_zip.cmake: ${var} is not set")
    endif()
endforeach()

set(_name "minilog-${VERSION}")
set(_stage "${OUTPUT_DIR}/zip-stage")
set(_root "${_stage}/${_name}")
set(_archive "${OUTPUT_DIR}/${_name}-win64.zip")

# Start from nothing every time, so a file dropped from the list below does not
# linger in the staging directory and end up in the archive anyway.
file(REMOVE_RECURSE "${_stage}")
file(REMOVE "${_archive}")
file(MAKE_DIRECTORY "${_root}")

set(_files
    "${SERVER_EXE}"
    "${SEND_EXE}"
    "${SOURCE_DIR}/installer/minilog.conf"
    "${SOURCE_DIR}/src/cli-viewer/minilog-cli-viewer.py"
    "${SOURCE_DIR}/LICENSE"
    "${SOURCE_DIR}/README.md"
    "${SOURCE_DIR}/CHANGES.md"
)
if(WEB_VIEWER_EXE)
    list(APPEND _files "${WEB_VIEWER_EXE}")
endif()
# The symbols go in beside the executable: a crash dump from a deployment is
# only readable with the .pdb of that exact build, and by the time one is
# wanted the build that produced it is not on anyone's machine any more.
if(SERVER_PDB AND EXISTS "${SERVER_PDB}")
    list(APPEND _files "${SERVER_PDB}")
endif()

foreach(_file IN LISTS _files)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "package_zip.cmake: ${_file} does not exist")
    endif()
    file(COPY "${_file}" DESTINATION "${_root}")
endforeach()

# Shipped under the name the viewer looks for, as the installer ships it, so
# that a directory copied whole from the archive is already a working layout.
configure_file("${SOURCE_DIR}/src/cli-viewer/minilog-cli-viewer.conf.example"
               "${_root}/minilog-cli-viewer.conf" COPYONLY)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${_archive}" --format=zip "${_name}"
    WORKING_DIRECTORY "${_stage}"
    RESULT_VARIABLE _result
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "package_zip.cmake: creating ${_archive} failed (${_result})")
endif()
message(STATUS "Wrote ${_archive}")
