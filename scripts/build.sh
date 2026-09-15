#!/usr/bin/env bash
# Configures and builds StreamForge with the required Clang 20 toolchain.
# Dependency sources are cached in .deps/cache so repeat builds work offline.
set -euo pipefail
cd "$(dirname "$0")/.."

CC=clang-20 CXX=clang++-20 cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build --parallel
