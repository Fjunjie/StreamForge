#!/usr/bin/env bash
# Coverage build + report (llvm-cov source-based coverage).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${1:-build-cov}

CC=clang-20 CXX=clang++-20 cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DSTREAMFORGE_COVERAGE=ON
cmake --build "$BUILD" --parallel

rm -f "$BUILD"/*.profraw "$BUILD"/sf.profdata
cd "$BUILD"
LLVM_PROFILE_FILE=sf.profraw ctest -R streamforge-all --output-on-failure
llvm-profdata-20 merge -sparse sf.profraw -o sf.profdata
llvm-cov-20 report ./streamforge-tests -instr-profile=sf.profdata \
    $(find ../src ../include -name '*.cpp' -o -name '*.hpp') | tail -40
llvm-cov-20 show ./streamforge-tests -instr-profile=sf.profdata \
    -format=html -output-dir coverage-html || true
echo "coverage.sh: report in $BUILD/coverage-html (if generated)"
