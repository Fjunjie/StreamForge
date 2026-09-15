#include <catch2/catch_test_macros.hpp>

#include "streamforge/core/error.hpp"
#include "streamforge/core/uuid.hpp"

using namespace streamforge;

TEST_CASE("error context chaining", "[error]") {
    Error err = Error::make(ErrorCode::IoOpen, "cannot open").ctx("path", "/tmp/x");
    REQUIRE(err.context.size() == 1);
    CHECK(err.context[0].first == "path");
    CHECK(err.code_name() == std::string("IoOpen"));
}

TEST_CASE("result carries value or error", "[error]") {
    Result<int> ok = 42;
    REQUIRE(ok.ok());
    CHECK(ok.value() == 42);

    Result<int> bad = Error::make(ErrorCode::InternalError, "boom");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::InternalError);

    Result<void> nothing = Result<void>::Ok();
    REQUIRE(nothing.ok());
    Result<void> fail = Result<void>::Err(Error::make(ErrorCode::DbBusy, "busy"));
    REQUIRE_FALSE(fail.ok());
}

TEST_CASE("uuid v4 shape and uniqueness", "[uuid]") {
    std::string a = uuid_v4();
    std::string b = uuid_v4();
    REQUIRE(a.size() == 36);
    CHECK(a[8] == '-');
    CHECK(a[13] == '-');
    CHECK(a[14] == '4'); // version nibble sits after the second hyphen group start
    CHECK(a[18] == '-');
    CHECK(a[23] == '-');
    CHECK(a != b);
}
