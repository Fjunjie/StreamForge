#include "streamforge/ingest/line_reader.hpp"

namespace streamforge {

bool LineReader::next(Line& out) {
    out.text.clear();
    out.truncated = false;
    out.bytes_consumed = 0;

    bool read_any = false;
    int c = in_.get();
    if (c == std::char_traits<char>::eof())
        return false;
    read_any = true;
    while (c != '\n' && c != std::char_traits<char>::eof()) {
        if (out.text.size() < max_line_bytes_) {
            out.text.push_back(static_cast<char>(c));
        } else {
            out.truncated = true; // keep draining, bounded memory
        }
        ++out.bytes_consumed;
        c = in_.get();
    }
    if (c == '\n')
        ++out.bytes_consumed;
    if (!read_any)
        return false;
    return true;
}

} // namespace streamforge
