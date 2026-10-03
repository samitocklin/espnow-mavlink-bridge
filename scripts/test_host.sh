#!/usr/bin/env bash
# Host unit tests (ASan + UBSan) for the hardware-independent components.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S test/host -B build/host -G Ninja >/dev/null
cmake --build build/host
ctest --test-dir build/host --output-on-failure
