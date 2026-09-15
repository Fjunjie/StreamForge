cmake_minimum_required(VERSION 3.20)

project(StreamForge VERSION 0.1.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerCheck)
sf_check_toolchain()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # consumed by clang-tidy
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug/Release/RelWithDebInfo/MinSizeRel)" FORCE)
endif()

option(STREAMFORGE_BUILD_TESTS "Build the streamforge-tests binary" ON)
option(STREAMFORGE_COVERAGE "Build with LLVM source-based coverage instrumentation" OFF)
option(STREAMFORGE_WERROR "Treat compiler warnings as errors" OFF)
set(STREAMFORGE_SANITIZE "" CACHE STRING "Comma separated sanitizers to enable: asan, ubsan")

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)      # TLM frame compression (used from M2); fail hard when missing.
find_package(OpenSSL REQUIRED)   # SHA-256 via EVP (file identity hashes).
find_package(SQLite3 REQUIRED)   # Persistence.
find_path(UUID_INCLUDE_DIR uuid/uuid.h REQUIRED) # libuuid for UUIDv4 identifiers.
find_library(UUID_LIBRARY NAMES uuid REQUIRED)
if(NOT UUID_INCLUDE_DIR OR NOT UUID_LIBRARY)
    message(FATAL_ERROR "libuuid development files not found; install uuid-dev (Ubuntu) and re-run cmake.")
endif()
add_library(sf_system_uuid INTERFACE)
target_include_directories(sf_system_uuid INTERFACE ${UUID_INCLUDE_DIR})
target_link_libraries(sf_system_uuid INTERFACE ${UUID_LIBRARY})

if(STREAMFORGE_SANITIZE)
    add_compile_options(-fno-omit-frame-pointer)
    string(REPLACE "," ";" _sanitizers "${STREAMFORGE_SANITIZE}")
    foreach(_flag ${_sanitizers})
        if(_flag STREQUAL "asan")
            add_compile_options(-fsanitize=address)
            add_link_options(-fsanitize=address)
        elseif(_flag STREQUAL "ubsan")
            add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
            add_link_options(-fsanitize=undefined)
        else()
            message(FATAL_ERROR "Unknown sanitizer '${_flag}'; supported values: asan, ubsan")
        endif()
    endforeach()
endif()

if(STREAMFORGE_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

include(Dependencies)

# ---------------------------------------------------------------------------
# Embedded database migrations (generated header, rebuilt when *.sql changes)
# ---------------------------------------------------------------------------
set(SF_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SF_GENERATED_DIR}")
set(SF_MIGRATIONS_HEADER "${SF_GENERATED_DIR}/migrations_embedded.hpp")
file(GLOB SF_MIGRATION_FILES "${CMAKE_CURRENT_SOURCE_DIR}/migrations/*.sql")
list(SORT SF_MIGRATION_FILES)
if(NOT SF_MIGRATION_FILES)
    message(FATAL_ERROR "No migration SQL files found under migrations/.")
endif()
add_custom_command(
    OUTPUT "${SF_MIGRATIONS_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DMIGRATIONS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/migrations
        -DOUT_FILE=${SF_MIGRATIONS_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedMigrations.cmake
    DEPENDS ${SF_MIGRATION_FILES}
    COMMENT "Embedding database migrations"
    VERBATIM)
add_custom_target(sf_generate_migrations DEPENDS "${SF_MIGRATIONS_HEADER}")

# ---------------------------------------------------------------------------
# Warning flags apply to project targets only; third-party code stays quiet.
# ---------------------------------------------------------------------------
function(sf_add_warnings tgt)
    # -Wno-deprecated-literal-operator silences a vendor-header warning emitted from
    # Howard Hinnant date/date.h (third-party code included by our TUs); project-owned
    # code stays fully covered by -Wall -Wextra -Wpedantic.
    target_compile_options(${tgt} PRIVATE
        -Wall -Wextra -Wpedantic -Wno-deprecated-literal-operator
        $<$<BOOL:${STREAMFORGE_WERROR}>:-Werror>)
endfunction()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
add_library(sf_core STATIC
    src/core/error.cpp
    src/core/time.cpp
    src/core/uuid.cpp
    src/core/log.cpp
    src/core/fs_util.cpp)
target_include_directories(sf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sf_core
    PUBLIC spdlog::spdlog date::date-tz sf_system_uuid Threads::Threads
    PRIVATE ZLIB::ZLIB)
sf_add_warnings(sf_core)

add_library(sf_config STATIC src/config/config.cpp)
target_link_libraries(sf_config PUBLIC sf_core yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json date::date-tz)
sf_add_warnings(sf_config)

add_library(sf_storage STATIC
    src/storage/db.cpp
    src/storage/migrations.cpp
    src/storage/store.cpp)
target_include_directories(sf_storage PRIVATE "${SF_GENERATED_DIR}")
add_dependencies(sf_storage sf_generate_migrations)
target_link_libraries(sf_storage PUBLIC sf_config PRIVATE SQLite::SQLite3)
sf_add_warnings(sf_storage)

add_library(sf_ingest STATIC
    src/ingest/format.cpp
    src/ingest/line_reader.cpp
    src/ingest/csv/csv_parser.cpp
    src/ingest/jsonl/jsonl_parser.cpp
    src/ingest/validation.cpp
    src/ingest/file_identity.cpp
    src/ingest/discovery.cpp
    src/ingest/archive.cpp
    src/ingest/error_report.cpp
    src/ingest/pipeline.cpp)
target_link_libraries(sf_ingest PUBLIC sf_storage PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json)
sf_add_warnings(sf_ingest)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(streamforge-server src/server_main.cpp)
target_link_libraries(streamforge-server PRIVATE sf_ingest sf_storage sf_config sf_core)
sf_add_warnings(streamforge-server)

add_executable(streamforgectl src/ctl_main.cpp)
target_link_libraries(streamforgectl PRIVATE sf_ingest sf_storage sf_config sf_core)
sf_add_warnings(streamforgectl)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(STREAMFORGE_BUILD_TESTS)
    enable_testing()
    add_executable(streamforge-tests
        tests/support/test_env.cpp
        tests/unit/test_time.cpp
        tests/unit/test_error.cpp
        tests/unit/test_bounded_queue.cpp
        tests/unit/test_log.cpp
        tests/unit/test_fs_util.cpp
        tests/unit/test_csv.cpp
        tests/unit/test_jsonl.cpp
        tests/unit/test_validation.cpp
        tests/unit/test_config.cpp
        tests/unit/test_storage.cpp
        tests/unit/test_ingest.cpp
        tests/unit/test_pipeline.cpp
        tests/integration/test_cli.cpp)
    target_include_directories(streamforge-tests PRIVATE tests)
    target_compile_definitions(streamforge-tests PRIVATE
        STREAMFORGE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        STREAMFORGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        STREAMFORGE_CTL_BIN="$<TARGET_FILE:streamforgectl>")
    target_link_libraries(streamforge-tests PRIVATE
        Catch2::Catch2WithMain sf_ingest sf_storage sf_config sf_core)
    sf_add_warnings(streamforge-tests)
    add_test(NAME streamforge-all COMMAND streamforge-tests)
endif()

# ---------------------------------------------------------------------------
# clang-tidy / clang-format helpers
# ---------------------------------------------------------------------------
find_program(CLANG_TIDY_EXE NAMES clang-tidy-20 clang-tidy)
find_program(CLANG_FORMAT_EXE NAMES clang-format-20 clang-format)
add_custom_target(tidy
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DCLANG_TIDY_EXE=${CLANG_TIDY_EXE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunTidy.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    VERBATIM)
add_custom_target(format
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DCLANG_FORMAT_EXE=${CLANG_FORMAT_EXE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFormat.cmake
    VERBATIM)
