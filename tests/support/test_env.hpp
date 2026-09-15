#pragma once

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"

namespace sf_test {

namespace fs = std::filesystem;

// Unique temporary directory removed on destruction.
struct TempDir {
    std::string path;
    TempDir();
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string sub(const std::string& name) const; // creates and returns root/name
    [[nodiscard]] std::string file(const std::string& name, const std::string& content) const;
};

// A valid configuration for pipeline tests: 3 metrics, 2 devices, small batches.
// Layout: root/{input,archive,quarantine,reports,log}, database root/test.db.
[[nodiscard]] std::shared_ptr<const streamforge::ConfigSnapshot> make_config(const std::string& root,
                                                                             int64_t batch_size = 3);

// Unwraps Result<optional<T>> for tests: fails the test when absent or errored, otherwise
// returns a reference to the contained value.
template <typename T> [[nodiscard]] const T& value_or_fail(const streamforge::Result<std::optional<T>>& result) {
    const auto& opt = result.value();
    REQUIRE(result.ok());
    REQUIRE(opt.has_value());
    return *opt;
}

} // namespace sf_test
