#include "streamforge/ingest/format.hpp"

namespace streamforge {

InputFormat format_from_extension(const std::string& ext) {
    if (ext == "csv")
        return InputFormat::Csv;
    if (ext == "jsonl" || ext == "json")
        return InputFormat::Jsonl;
    if (ext == "tlm")
        return InputFormat::Tlm;
    return InputFormat::Unknown;
}

const char* format_name(InputFormat fmt) {
    switch (fmt) {
    case InputFormat::Csv:
        return "csv";
    case InputFormat::Jsonl:
        return "jsonl";
    case InputFormat::Tlm:
        return "tlm";
    case InputFormat::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string strip_ready_suffix(const std::string& name) {
    const std::string suffix = ".ready";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return name.substr(0, name.size() - suffix.size());
    }
    return name;
}

} // namespace streamforge
