# Runs clang-tidy over the project's own C/C++ sources and public headers.
# Expects: SOURCE_DIR, CLANG_TIDY_EXE.
cmake_minimum_required(VERSION 3.20)

if(NOT CLANG_TIDY_EXE)
    message(FATAL_ERROR "clang-tidy-20 not found; install clang-tidy-20 (apt.llvm.org).")
endif()

file(GLOB_RECURSE _sources
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/include/*.hpp"
    "${SOURCE_DIR}/tests/*.cpp"
    "${SOURCE_DIR}/tests/*.hpp")

execute_process(
    COMMAND ${CLANG_TIDY_EXE} --quiet -p "${SOURCE_DIR}/build" ${_sources}
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)

if(NOT _rc EQUAL 0)
    message(STATUS "${_out}")
    message(STATUS "${_err}")
    message(FATAL_ERROR "clang-tidy reported problems (exit ${_rc}).")
endif()
message(STATUS "clang-tidy: clean")
