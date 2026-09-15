# Third-party dependency management.
#
# Policy (agreed for this project):
#   - System libraries: ZLIB, OpenSSL, SQLite3, libuuid (resolved in the top-level CMakeLists
#     with REQUIRED find_* calls so a missing dependency fails configuration instead of
#     silently disabling functionality).
#   - Everything else: FetchContent with a fixed tag/commit; sources are cached under
#     .deps/cache in the source tree so that repeat builds work offline after the first fetch.
include(FetchContent)

# Default dependency cache lives in the source tree so repeat builds work offline after the
# first fetch. Pass -DFETCHCONTENT_BASE_DIR=<dir> to relocate it (e.g. when the source tree
# sits on a filesystem that does not support all file operations, like a Windows drive
# mounted in WSL).
if(NOT FETCHCONTENT_BASE_DIR OR FETCHCONTENT_BASE_DIR STREQUAL "${CMAKE_BINARY_DIR}/_deps")
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps/cache" CACHE PATH
        "FetchContent download/build cache (kept outside build/ for offline rebuilds)" FORCE)
endif()
set(FETCHCONTENT_QUIET OFF)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3)

set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1)

set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1)

# Howard Hinnant date/tz: C++17 time zone and DST handling backed by the OS tz database
# (/usr/share/zoneinfo). Uses the system IANA database; no bundled tzdata, no network.
set(BUILD_TZ_LIB ON CACHE BOOL "" FORCE)
set(USE_SYSTEM_TZ_DB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(date
    GIT_REPOSITORY https://github.com/HowardHinnant/date.git
    GIT_TAG        v3.0.1)

FetchContent_MakeAvailable(yaml-cpp nlohmann_json spdlog Catch2 date)
