#!/usr/bin/env bash
# Build libslock_twin.so (Linux, e.g. CI): the real firmware core + simulated mechanism and door,
# for twin.py, test_protocol.py and test_scenarios.py. On Windows use build.ps1.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
fw="$here/../firmware"
mkdir -p "$here/build"
g++ -std=c++17 -O2 -shared -fPIC -Wall -Wextra -Werror \
    -I "$fw/lib/slock_core/src" -I "$fw/test_host" \
    "$fw"/lib/slock_core/src/slock/*.cpp "$here/core/twin_api.cpp" -o "$here/build/libslock_twin.so"
echo "Built $here/build/libslock_twin.so"
