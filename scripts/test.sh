#!/usr/bin/env bash
# Runs the full test suite (or a Catch2 tag selection passed as arguments).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --build build --parallel
ctest --test-dir build --output-on-failure "$@"
