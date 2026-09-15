#pragma once

#include <string>

namespace streamforge {

enum class InputFormat { Csv, Jsonl, Tlm, Unknown };

// Maps a file extension (already lowercase, without the dot) to a format.
// ".tlm" is recognized but only processed from M2 onwards.
InputFormat format_from_extension(const std::string& ext);

const char* format_name(InputFormat fmt);

// Strips a trailing ".ready" suffix from a file name (FR-IN-002).
std::string strip_ready_suffix(const std::string& name);

} // namespace streamforge
