#pragma once

#include <string>

namespace streamforge {

// Generates a random UUID v4 (lowercase, hyphenated) using libuuid.
std::string uuid_v4();

} // namespace streamforge
