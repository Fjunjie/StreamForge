# Applies clang-format to the project's own C/C++ sources and public headers.
# Expects: SOURCE_DIR, CLANG_FORMAT_EXE.
cmake_minimum_required(VERSION 3.20)

if(NOT CLANG_FORMAT_EXE)
    message(FATAL_ERROR "clang-format-20 not found; install clang-format-20 (apt.llvm.org).")
endif()

file(GLOB_RECURSE _sources
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/include/*.hpp"
    "${SOURCE_DIR}/tests/*.cpp"
    "${SOURCE_DIR}/tests/*.hpp")

execute_process(
    COMMAND ${CLANG_FORMAT_EXE} -i ${_sources}
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "clang-format failed: ${_err}")
endif()
message(STATUS "clang-format: applied")
