#!/usr/bin/env bash
#
# What every Linux job needs to build minilog, plus whatever it passes in
# (clang for the sanitizer, TSan and libFuzzer jobs; lcov for the coverage one).
#
# The Boost components are named one by one rather than taken from
# libboost-all-dev, which depends on libboost-mpi-dev, libboost-mpi-python-dev,
# libboost-graph-parallel-dev and libboost-python-dev, and so installs an MPI
# runtime — libfabric, libpmix, librdmacm, libibumad, libibmad, libucx, hwloc,
# munge, an OpenCL loader — that minilog does not link and never has.  Those
# were not merely wasted bandwidth: on 2026-11-10 they were the packages the
# archive was serving with 30-second retries, and the libFuzzer job twice spent
# its whole timeout in apt without compiling a line, while the TSan job
# installed the same set in 81 seconds minutes later.  Fewer packages is less
# exposure to that.
#
# The list has to match what CMakeLists.txt asks find_package(Boost) for, which
# is json, program_options and unit_test_framework, plus the header-only Asio
# and PropertyTree out of libboost-dev.  Getting it wrong fails at configure
# time, naming the component — it cannot pass quietly.

set -euo pipefail

sudo apt-get update -qq
sudo apt-get install -y \
    ninja-build \
    zlib1g-dev \
    libboost-dev \
    libboost-json-dev \
    libboost-program-options-dev \
    libboost-test-dev \
    "$@"
