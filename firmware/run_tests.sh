#!/usr/bin/env bash
# Build and run the host unit tests for the portable core (lib/slock_core) with g++ (Linux, macOS,
# or MSYS2). Same flags as run_tests.ps1.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here/build"
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
    -I "$here/lib/slock_core/src" -I "$here/test_host" \
    "$here"/lib/slock_core/src/slock/*.cpp "$here"/test_host/*.cpp -o "$here/build/host_tests"
"$here/build/host_tests"
