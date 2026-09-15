#!/usr/bin/env bash
# Local CI: format check + build (with warnings as errors) + full test suite.
# Run inside the supported toolchain environment (WSL2 Ubuntu 24.04 / Linux x86_64).
set -euo pipefail
cd "$(dirname "$0")/.."

if command -v clang-format-20 >/dev/null 2>&1; then
    echo "== clang-format check =="
    find src include tests -type f \( -name '*.hpp' -o -name '*.cpp' \) -print0 \
        | xargs -0 clang-format-20 --dry-run --Werror
else
    echo "clang-format-20 not found; skipping format check"
fi

echo "== build (Release, warnings as errors) =="
CC=clang-20 CXX=clang++-20 cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DSTREAMFORGE_WERROR=ON "$@"
cmake --build build --parallel

echo "== tests =="
ctest --test-dir build --output-on-failure

echo "ci.sh: all checks passed"
